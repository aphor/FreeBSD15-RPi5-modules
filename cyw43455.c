/*
 * cyw43455.c — CYW43455 SDIO WiFi driver: probe/attach/detach, sysctl, module
 *
 * Attaches to the sdiob(4) newbus bridge on the WiFi SDIO slot.  The sdiob
 * driver enumerates one child per SDIO function; this driver claims function 1
 * (the backplane function) and locates function 2 (WLAN data) as a sibling.
 *
 * Milestone 1 goal: load firmware, read firmware version string via IOVAR.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>

#include "cyw43455_var.h"

MALLOC_DEFINE(M_CYW43455, "cyw43455", "CYW43455 SDIO WiFi driver");

/*
 * Read-only attach-time diagnostic.  Set hw.cyw43455.probe_fwsup=1 in
 * /boot/loader.conf (or `kenv hw.cyw43455.probe_fwsup=1` before kldload)
 * to query the firmware's "sup_wpa" iovar at attach time.  Logs GET +
 * SET(1) + SET(0-restore) returns.  No effect on the join path.
 * See plan floofy-whistling-scott.md Step 1.
 */
int cyw_probe_fwsup_tunable = 0;
TUNABLE_INT("hw.cyw43455.probe_fwsup", &cyw_probe_fwsup_tunable);

void
cyw_probe_fwsup(struct cyw_softc *sc)
{
	uint32_t v = 0xdeadbeef;
	int err;

	device_printf(sc->dev, "probe_fwsup: begin (Linux FWSUP detector)\n");

	err = cyw_fil_iovar_int_get(sc, "sup_wpa", &v);
	device_printf(sc->dev,
	    "probe_fwsup: GET sup_wpa returned %d value=0x%x\n", err, v);

	err = cyw_fil_iovar_int_set(sc, "sup_wpa", 1);
	device_printf(sc->dev,
	    "probe_fwsup: SET sup_wpa=1 returned %d\n", err);

	err = cyw_fil_iovar_int_set(sc, "sup_wpa", 0);
	device_printf(sc->dev,
	    "probe_fwsup: SET sup_wpa=0 returned %d (restore)\n", err);

	device_printf(sc->dev, "probe_fwsup: end\n");
}

/* -------------------------------------------------------------------------
 * Firmware security-state read-back sysctls
 *
 * Read-only windows into what the *firmware* actually holds for the primary
 * BSS security configuration, independent of what wpa_supplicant believes it
 * set.  Each handler issues a live iovar GET; these run in sleepable process
 * context (sysctl), so the SDIO transaction is safe, and cyw_fil_iovar_int_get
 * already serializes on the F2 lock.  Only valid once the SDPCM layer is up.
 *
 *   hw.cyw43455.fw_wsec      — wsec     (cipher suite mask; 4 = AES-CCM)
 *   hw.cyw43455.fw_wpa_auth  — wpa_auth (key-mgmt mask; 0x80 = WPA2-PSK)
 *   hw.cyw43455.fw_auth      — auth     (0 = open system)
 * ------------------------------------------------------------------------- */
static int
cyw_sysctl_fw_iovar(struct cyw_softc *sc, const char *iovar,
    struct sysctl_oid *oidp, struct sysctl_req *req)
{
	uint32_t v = 0;
	int err;

	if (!sc->sdpcm_running)
		return (ENXIO);
	err = cyw_fil_iovar_int_get(sc, iovar, &v);
	if (err != 0)
		return (err);
	return (sysctl_handle_int(oidp, &v, 0, req));
}

static int
cyw_sysctl_fw_wsec(SYSCTL_HANDLER_ARGS)
{
	return (cyw_sysctl_fw_iovar(arg1, "wsec", oidp, req));
}

static int
cyw_sysctl_fw_wpa_auth(SYSCTL_HANDLER_ARGS)
{
	return (cyw_sysctl_fw_iovar(arg1, "wpa_auth", oidp, req));
}

static int
cyw_sysctl_fw_auth(SYSCTL_HANDLER_ARGS)
{
	return (cyw_sysctl_fw_iovar(arg1, "auth", oidp, req));
}

/* -------------------------------------------------------------------------
 * Probe
 * ------------------------------------------------------------------------- */
static int
cyw_probe(device_t dev)
{
	/* We only drive F1 (backplane). Reject F0, F2, F3. */
	if (sdio_get_funcnum(dev) != 1)
		return (ENXIO);
	if (sdio_get_vendor(dev) != CYW_VENDOR_BROADCOM)
		return (ENXIO);

	device_set_desc(dev, "Cypress CYW43455 802.11ac SDIO WiFi");
	return (BUS_PROBE_DEFAULT);
}

/* -------------------------------------------------------------------------
 * Attach
 * ------------------------------------------------------------------------- */
static int
cyw_attach(device_t dev)
{
	struct cyw_softc *sc;
	device_t parent, *children;
	int i, nchildren, err;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->ram_base = CYW_RAM_BASE;
	sc->ram_size = CYW_RAM_SIZE;

	mtx_init(&sc->mtx, "cyw43455", NULL, MTX_DEF);
	sx_init(&sc->ioctl_sx, "cyw43455_ioctl");

	/* Get our F1 sdio_func */
	sc->f1 = sdio_get_function(dev);
	if (sc->f1 == NULL) {
		device_printf(dev, "failed to get F1 function\n");
		err = ENXIO;
		goto fail_mtx;
	}

	/* Find F2 sibling on the same sdiob parent */
	parent = device_get_parent(dev);
	if (device_get_children(parent, &children, &nchildren) == 0) {
		for (i = 0; i < nchildren; i++) {
			if (sdio_get_funcnum(children[i]) == 2) {
				sc->f2 = sdio_get_function(children[i]);
				break;
			}
		}
		free(children, M_TEMP);
	}
	if (sc->f2 == NULL) {
		device_printf(dev, "could not find F2 sibling\n");
		err = ENXIO;
		goto fail_mtx;
	}

	/* sysctl tree at hw.cyw43455 */
	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO, "cyw43455",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "CYW43455 WiFi");

	SYSCTL_ADD_U16(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "chip_id", CTLFLAG_RD, &sc->chip_id, 0, "Chip ID");
	SYSCTL_ADD_U8(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "chip_rev", CTLFLAG_RD, &sc->chip_rev, 0, "Chip revision");
	SYSCTL_ADD_STRING(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "firmware_version", CTLFLAG_RD,
	    sc->fw_version, 0, "Firmware version string");

	/* RX diagnostic counters (Step 6 — F2 EIO classification) */
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_ok_count", CTLFLAG_RD, &sc->rx_ok_count, 0,
	    "Successful F2 reads since attach");
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_eio_count", CTLFLAG_RD, &sc->rx_eio_count, 0,
	    "F2 CMD53 reads that returned EIO");
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_eagain_count", CTLFLAG_RD, &sc->rx_eagain_count, 0,
	    "F2 reads bounced by gate or header check");
	SYSCTL_ADD_INT(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_last_ok_ticks", CTLFLAG_RD, &sc->rx_last_ok_ticks, 0,
	    "ticks of last successful F2 read");
	SYSCTL_ADD_INT(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_last_eio_ticks", CTLFLAG_RD, &sc->rx_last_eio_ticks, 0,
	    "ticks of last F2 EIO");

	/* Data-channel RX counters (Step 7 — RX path verification) */
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_data_frames", CTLFLAG_RD, &sc->rx_data_frames, 0,
	    "SDPCM channel-2 frames delivered to net80211");
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_data_bytes", CTLFLAG_RD, &sc->rx_data_bytes, 0,
	    "Total bytes delivered on channel 2");
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "rx_eapol_frames", CTLFLAG_RD, &sc->rx_eapol_frames, 0,
	    "EAPOL frames (EtherType 0x888E) delivered up");

	/* TX counters — 4-way handshake diagnosis */
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "tx_data_frames", CTLFLAG_RD, &sc->tx_data_frames, 0,
	    "Frames handed to cyw_transmit");
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "tx_eapol_frames", CTLFLAG_RD, &sc->tx_eapol_frames, 0,
	    "TX subset with EtherType 0x888E");
	SYSCTL_ADD_U64(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
	    "tx_eapol_bytes", CTLFLAG_RD, &sc->tx_eapol_bytes, 0,
	    "TX EAPOL byte total");

	/* Firmware security-state read-back (live iovar GET) */
	SYSCTL_ADD_PROC(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO, "fw_wsec",
	    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    cyw_sysctl_fw_wsec, "I", "firmware wsec value (live GET)");
	SYSCTL_ADD_PROC(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO, "fw_wpa_auth",
	    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    cyw_sysctl_fw_wpa_auth, "I", "firmware wpa_auth value (live GET)");
	SYSCTL_ADD_PROC(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO, "fw_auth",
	    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
	    cyw_sysctl_fw_auth, "I", "firmware auth value (live GET)");

	/* SDIO attach: enable F1, enable clock, read chip ID */
	err = cyw_sdio_attach(sc);
	if (err != 0) {
		device_printf(dev, "SDIO attach failed: %d\n", err);
		goto fail_sysctl;
	}

	device_printf(dev, "chip 0x%04x rev %d\n", sc->chip_id, sc->chip_rev);

	/* Seed initial TX credits before fw_download calls cyw_iovar_get. */
	sc->sdpcm_rx_max = 4;

	/* Download firmware + NVRAM and boot the chip */
	err = cyw_fw_download(sc);
	if (err != 0) {
		device_printf(dev, "firmware download failed: %d\n", err);
		goto fail_sdio;
	}

	/*
	 * Disable RX glom (SDPCM channel 3 superframes).  CYW43455's SDIO core
	 * is rev >= 12 and firmware defaults to glom-enabled, packing multiple
	 * RX frames into a single SDIO transfer.  Our SDPCM RX path doesn't
	 * de-glom, so we'd see "unknown channel 3" + desync + EIO storms once
	 * the firmware starts emitting scan results.  In brcmfmac terminology
	 * "bus:txglom" is the device-to-host direction (sdio.c:3743), i.e. our
	 * RX.  Failure is non-fatal — older firmware may not implement it.
	 */
	if (cyw_fil_iovar_int_set(sc, "bus:txglom", 0) != 0)
		device_printf(dev, "cyw_attach: bus:txglom disable failed (non-fatal)\n");

	/*
	 * Dongle init IOVARs — issued in the boot-time window (sdpcm_running
	 * still false) using the polling path.  This avoids a race: on fresh
	 * boot the SR init completes and firmware events immediately flood F2;
	 * if these IOVARs were issued after cyw_sdpcm_attach() starts the RX
	 * callout, concurrent CMD53 read (callout) and CMD53 write (IOVAR TX)
	 * corrupt the SDIO CAM queue → camq_remove panic.
	 *
	 * On reload after kldunload (chip went through ARM halt, SR not re-
	 * inited), INTSTATUS lacks data-available bits so the poll path returns
	 * EAGAIN; these are non-fatal and print a warning.
	 *
	 * Mirrors brcmf_dongle_init() in Linux brcmfmac/cfg80211.c.
	 */
	if (cyw_fil_iovar_int_set(sc, "roam_off", 1) != 0)
		device_printf(dev, "cyw_attach: roam_off IOVAR failed\n");
	if (cyw_fil_iovar_int_set(sc, "pm", 0) != 0)
		device_printf(dev, "cyw_attach: pm IOVAR failed\n");
	if (cyw_fil_iovar_int_set(sc, "btc_mode", 0) != 0)
		device_printf(dev, "cyw_attach: btc_mode IOVAR failed\n");
	/* mpc intentionally left at firmware default (1 = enabled).
	 * NEED_MPC quirk does not apply to chip 0x4345 (CYW43455). */
	if (cyw_fil_iovar_int_set(sc, "allmulti", 1) != 0)
		device_printf(dev, "cyw_attach: allmulti IOVAR failed\n");

	/*
	 * BSS pre-configuration — boot-time polling window (sdpcm_running false).
	 *
	 * WLC_DOWN(1): put BSS in a clean initial state.
	 * WLC_SET_INFRA(1): select infrastructure (STA) mode.
	 *
	 * WLC_UP is intentionally omitted here.  Linux brcmfmac calls WLC_UP
	 * from brcmf_config_dongle() when the interface is first opened
	 * (ifconfig wlan0 up → __brcmf_cfg80211_up → brcmf_config_dongle).
	 * Calling it during attach (before events are flowing and the RX task
	 * is running) does not reliably bring the BSS up in the firmware's
	 * internal state — escan still returns BCME_NOTUP.
	 *
	 * cyw_parent() calls WLC_UP on first ic_nrunning > 0, exactly when
	 * Linux does.  The dongle_up flag ensures it is called only once.
	 */
	/*
	 * WLC_DOWN intentionally omitted: Linux brcmfmac never issues
	 * WLC_DOWN at attach time for STA mode.  C_DOWN only appears in
	 * the Linux AP-mode and P2P paths.  Issuing it here may push the
	 * BSS into a DOWN state that WLC_UP cannot cleanly recover from
	 * before escan is attempted (suspected cause of BCME_NOTUP).
	 */
	/*
	 * WLC_SET_INFRA intentionally omitted: Linux brcmfmac never issues
	 * WLC_SET_INFRA at attach time.  It only issues it from
	 * brcmf_cfg80211_change_iface() called inside brcmf_config_dongle()
	 * on the first ifconfig up, and always after BRCMF_C_UP.
	 */

	/*
	 * CLM blob upload — must happen after firmware is running and
	 * responding to IOVARs, but before cyw_sdpcm_attach() starts the
	 * RX callout.  Mirrors brcmf_c_process_clm_blob() in Linux common.c,
	 * which is called from brcmf_c_preinit_dcmds() at attach time.
	 * Non-fatal if the blob is absent (limited channels only).
	 */
	err = cyw_clm_load(sc);
	if (err != 0) {
		device_printf(dev, "CLM blob load failed: %d\n", err);
		goto fail_sdio;
	}

	/*
	 * Firmware version and net80211 setup — issued while sdpcm_running is
	 * still false (boot-time polling path).
	 *
	 * The 5 dongle init IOVARs above trigger the firmware's SR init
	 * sequence, which sets TOHOST=0x00040008 (HMB_DATA_FWREADY) and
	 * populates INTSTATUS with data-available bits.  The boot-time
	 * polling path in cyw_sdpcm_recv_one() gates on these bits, so it
	 * now works on both fresh-boot and reload-after-kldunload.
	 *
	 * cyw_sdpcm_attach() is called last, AFTER all SDIO transactions
	 * complete.  This guarantees no concurrent CMD53 access between the
	 * RX callout and the attach thread, which would corrupt the SDIO CAM
	 * queue and trigger a camq_remove panic.
	 */
	memset(sc->fw_version, 0, sizeof(sc->fw_version));
	if (cyw_fil_iovar_data_get(sc, "ver",
	    sc->fw_version, sizeof(sc->fw_version) - 1) != 0)
		strlcpy(sc->fw_version, "unknown", sizeof(sc->fw_version));
	device_printf(dev, "firmware: %s\n", sc->fw_version);

	/*
	 * net80211 attach — reads cur_etheraddr and writes event_msgs via
	 * the boot-time SDIO polling path (sdpcm_running still false).
	 * ieee80211_ifattach() itself performs no SDIO operations.
	 */
	err = cyw_cfg_attach(sc);
	if (err != 0) {
		device_printf(dev, "cfg attach failed: %d\n", err);
		goto fail_sdio;
	}

	/* SDPCM layer: start RX callout; sets sdpcm_running = true */
	err = cyw_sdpcm_attach(sc);
	if (err != 0) {
		device_printf(dev, "SDPCM attach failed: %d\n", err);
		goto fail_cfg;
	}

	/*
	 * WLC_UP is intentionally NOT issued here.  Linux brcmfmac never
	 * calls C_UP during attach; it defers to brcmf_config_dongle()
	 * which runs from ndo_open (first ifconfig up).  cyw_parent() is
	 * the FreeBSD equivalent and issues WLC_UP on first ic_nrunning > 0.
	 */

	return (0);

fail_cfg:
	cyw_cfg_detach(sc);
fail_sdio:
	cyw_sdio_detach(sc);
fail_sysctl:
	sysctl_ctx_free(&sc->sysctl_ctx);
fail_mtx:
	mtx_destroy(&sc->mtx);
	return (err);
}

/* -------------------------------------------------------------------------
 * Detach
 * ------------------------------------------------------------------------- */
static int
cyw_detach(device_t dev)
{
	struct cyw_softc *sc = device_get_softc(dev);

	cyw_cfg_detach(sc);	/* ieee80211_ifdetach before SDPCM stops */
	cyw_sdpcm_detach(sc);	/* stop callout; wake any sleeping fwil */
	cyw_sdio_detach(sc);
	sysctl_ctx_free(&sc->sysctl_ctx);
	sx_destroy(&sc->ioctl_sx);
	mtx_destroy(&sc->mtx);
	return (0);
}

/* -------------------------------------------------------------------------
 * Driver registration
 * ------------------------------------------------------------------------- */
static device_method_t cyw_methods[] = {
	DEVMETHOD(device_probe,		cyw_probe),
	DEVMETHOD(device_attach,	cyw_attach),
	DEVMETHOD(device_detach,	cyw_detach),
	DEVMETHOD_END
};

static driver_t cyw_driver = {
	"cyw43455",
	cyw_methods,
	sizeof(struct cyw_softc),
};

DRIVER_MODULE(cyw43455, sdiob, cyw_driver, NULL, NULL);
MODULE_DEPEND(cyw43455, sdiob, 1, 1, 1);
MODULE_DEPEND(cyw43455, wlan, 1, 1, 1);
MODULE_VERSION(cyw43455, 1);
