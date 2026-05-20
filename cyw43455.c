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
	 * Firmware initialisation IOVARs — issued in the boot-time path,
	 * *before* cyw_sdpcm_attach() starts the RX callout.  This avoids
	 * the camq_remove panic: some IOVARs (pm=0, mpc=0, btc_mode=0)
	 * trigger asynchronous firmware events.  If issued after the callout
	 * starts, cyw_sdpcm_task drains those events via CMD53 read while
	 * the attach thread simultaneously writes the next IOCTL via CMD53,
	 * corrupting the SDIO CAM queue.
	 *
	 * cyw_fw_download() exits with the firmware running but the callout
	 * not yet started, so single-threaded SDIO access is guaranteed here.
	 *
	 * Mirrors brcmf_dongle_init() (brcmfmac/cfg80211.c).
	 * Failures are non-fatal: firmware may not support all IOVARs.
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

	/* SDPCM layer: start RX callout; sets sdpcm_running = true */
	err = cyw_sdpcm_attach(sc);
	if (err != 0) {
		device_printf(dev, "SDPCM attach failed: %d\n", err);
		goto fail_sdio;
	}

	/* Read firmware version string via runtime condvar path. */
	memset(sc->fw_version, 0, sizeof(sc->fw_version));
	if (cyw_fil_iovar_data_get(sc, "ver",
	    sc->fw_version, sizeof(sc->fw_version) - 1) != 0)
		strlcpy(sc->fw_version, "unknown", sizeof(sc->fw_version));
	device_printf(dev, "firmware: %s\n", sc->fw_version);

	/* net80211 layer: read MAC, ifattach; uses condvar IOVAR path */
	err = cyw_cfg_attach(sc);
	if (err != 0) {
		device_printf(dev, "cfg attach failed: %d\n", err);
		goto fail_sdpcm;
	}

	return (0);

fail_sdpcm:
	cyw_sdpcm_detach(sc);
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
