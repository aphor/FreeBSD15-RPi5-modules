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
#include <sys/firmware.h>
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
	if (cyw_fil_iovar_int_set(sc, "mpc", 0) != 0)
		device_printf(dev, "cyw_attach: mpc IOVAR failed\n");
	if (cyw_fil_iovar_int_set(sc, "allmulti", 1) != 0)
		device_printf(dev, "cyw_attach: allmulti IOVAR failed\n");

	/*
	 * BSS bring-up sequence — mirrors brcmf_cfg_attach() in the reference
	 * driver (freebsd-brcmfmac/src/cfg.c).  Must run here in the boot-time
	 * polling window (sdpcm_running still false) before cyw_sdpcm_attach()
	 * starts the RX callout and claims exclusive F2 ownership.
	 *
	 * WLC_DOWN(1): put BSS in a clean initial state.
	 * WLC_SET_INFRA(1): select infrastructure (STA) mode; required before
	 *   the firmware will accept scan or join commands.
	 * WLC_UP(0): bring the BSS up; initialises PHY, opens the radio.
	 * 200 ms pause: CYW firmware needs time after WLC_UP before join works
	 *   (empirically required; brcmfmac comment: "CYW firmware needs time
	 *   after C_UP before join works").
	 *
	 * NOTE: do NOT repeat WLC_UP in cyw_parent or cyw_do_escan.  The
	 * reference driver comment warns: "Repeating C_UP here triggers a
	 * redundant wl_open in the firmware that re-runs PHY init."
	 */
	{
		int r;
		uint32_t isup;

		r = cyw_fil_cmd_int_set(sc, WLC_DOWN,      1);
		device_printf(dev, "cyw_attach: WLC_DOWN=%d\n", r);
		r = cyw_fil_cmd_int_set(sc, WLC_SET_INFRA, 1);
		device_printf(dev, "cyw_attach: WLC_SET_INFRA=%d\n", r);
		r = cyw_fil_cmd_int_set(sc, WLC_UP,        0);
		device_printf(dev, "cyw_attach: WLC_UP=%d\n", r);

		/*
		 * Read back BSS "up" state (cmd 19 == C_GET_UP / WLC_GET_INFRA).
		 * Reference: brcmfmac/cfg.c "isup=%u after bss_up" readback.
		 * isup == 1 confirms firmware set the BSS up flag.
		 */
		isup = 0;
		(void)cyw_fil_cmd_data_get(sc, 19, &isup, sizeof(isup));
		device_printf(dev, "cyw_attach: isup after WLC_UP=%u\n",
		    le32toh(isup));
	}
	pause("cyw_bssup", howmany(200 * hz, 1000));

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

	/* SDPCM layer: start RX callout last; sets sdpcm_running = true */
	err = cyw_sdpcm_attach(sc);
	if (err != 0) {
		device_printf(dev, "SDPCM attach failed: %d\n", err);
		goto fail_cfg;
	}

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
MODULE_DEPEND(cyw43455, firmware, 1, 1, 1);
MODULE_DEPEND(cyw43455, cyw43455fw, 1, 1, 1);
MODULE_DEPEND(cyw43455, wlan, 1, 1, 1);
MODULE_VERSION(cyw43455, 1);
