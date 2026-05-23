/*
 * cyw43455_cfg.c — net80211 integration layer
 *
 * Reads the chip MAC address, attaches to net80211, and provides the
 * minimum set of ic callbacks.  This milestone wires the driver into the
 * net80211 framework so the interface appears in ifconfig output.
 *
 * Milestones for remaining callbacks:
 *   scan_start / scan_end   — Milestone 2.3 (escan)
 *   iv_newstate association — Milestone 2.4/2.5
 *   ic_transmit data path   — Milestone 2.6
 *
 * Reference: /usr/src/sys/dev/bwn/if_bwn.c (SoftMAC FullMAC pattern)
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/systm.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ratectl.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"

#include "cyw43455_var.h"

/* -------------------------------------------------------------------------
 * Private VAP structure
 * ------------------------------------------------------------------------- */
struct cyw_vap {
	struct ieee80211vap	cv_vap;
	int			(*cv_newstate)(struct ieee80211vap *,
				    enum ieee80211_state, int);
};

/* -------------------------------------------------------------------------
 * State machine stub
 * ------------------------------------------------------------------------- */
static int
cyw_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	struct cyw_vap *cvap = (struct cyw_vap *)vap;
	struct ieee80211com *ic = vap->iv_ic;

	IEEE80211_UNLOCK(ic);
	/*
	 * Milestone 2.4/2.5: drive firmware association state machine here.
	 * For now pass straight through to the net80211 default handler.
	 */
	IEEE80211_LOCK(ic);

	return (cvap->cv_newstate(vap, nstate, arg));
}

/* -------------------------------------------------------------------------
 * VAP lifecycle
 * ------------------------------------------------------------------------- */
static struct ieee80211vap *
cyw_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ], int unit,
    enum ieee80211_opmode opmode, int flags,
    const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct cyw_vap *cvap;
	struct ieee80211vap *vap;

	if (!TAILQ_EMPTY(&ic->ic_vaps))
		return (NULL);		/* one VAP at a time */

	cvap = malloc(sizeof(*cvap), M_CYW43455, M_WAITOK | M_ZERO);
	vap  = &cvap->cv_vap;

	if (ieee80211_vap_setup(ic, vap, name, unit, opmode, flags,
	    bssid) != 0) {
		free(cvap, M_CYW43455);
		return (NULL);
	}

	cvap->cv_newstate = vap->iv_newstate;
	vap->iv_newstate  = cyw_newstate;

	ieee80211_ratectl_init(vap);
	/* ieee80211_vap_attach always returns 1; it panics on failure.
	 * Do not check the return value (bwn/iwm pattern). */
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	ic->ic_opmode = opmode;
	return (vap);
}

static void
cyw_vap_delete(struct ieee80211vap *vap)
{
	struct cyw_vap *cvap = (struct cyw_vap *)vap;

	ieee80211_ratectl_deinit(vap);
	ieee80211_vap_detach(vap);
	free(cvap, M_CYW43455);
}

/* -------------------------------------------------------------------------
 * Radio capabilities — 2.4 GHz B/G channels only for now.
 * 5 GHz and HT/VHT support added in later milestones.
 * ------------------------------------------------------------------------- */
static void
cyw_getradiocaps(struct ieee80211com *ic, int maxchans, int *nchans,
    struct ieee80211_channel chans[])
{
	uint8_t bands[IEEE80211_MODE_BYTES];

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	ieee80211_add_channels_default_2ghz(chans, maxchans, nchans, bands, 0);
}

/* -------------------------------------------------------------------------
 * Transmit — Milestone 2.6 data path stub (silently discard)
 * ------------------------------------------------------------------------- */
static int
cyw_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	m_freem(m);
	return (0);
}

/* -------------------------------------------------------------------------
 * Interface parent — mirrors brcmf_parent() + brcmf_config_dongle() in Linux.
 *
 * Linux brcmfmac calls WLC_UP from brcmf_config_dongle() the first time the
 * interface is opened (ifconfig wlan0 up → __brcmf_cfg80211_up).  Without
 * this, the CYW43455 firmware (7.45.x) returns BCME_NOTUP for the escan
 * IOVAR even though WLC_DOWN + WLC_SET_INFRA were issued during attach.
 *
 * The dongle_up flag mirrors cfg->dongle_up in Linux: WLC_UP runs exactly
 * once (first ic_nrunning 0→1 transition).  Repeating WLC_UP would trigger
 * redundant PHY re-init; the flag prevents that.
 *
 * Called by net80211 via ic_parent_task (taskqueue_thread), without IC lock.
 * Sleeping IOVARs (cyw_fil_*) are safe here.
 * ------------------------------------------------------------------------- */
static void
cyw_parent(struct ieee80211com *ic)
{
	struct cyw_softc *sc = ic->ic_softc;

	if (ic->ic_nrunning > 0) {
		if (!sc->dongle_up) {
			/*
			 * First-time interface up: apply the config_dongle sequence.
			 * WLC_UP was already called from cyw_attach() after the RX
			 * taskqueue started; it must not be repeated here.
			 *
			 * Mirrors brcmf_config_dongle() in Linux brcmfmac
			 * (cfg80211.c), called from __brcmf_cfg80211_up() on first
			 * ifconfig up.  Order matches Linux exactly:
			 *   1. Scan timing parameters
			 *   2. Power management off
			 *   3. Roam parameters
			 *   4. Re-assert infra/STA mode
			 *   5. Frameburst
			 */

			/* 1. Scan timing (brcmf_dongle_scantime) */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_SCAN_CHANNEL_TIME,
			    40);
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_SCAN_UNASSOC_TIME,
			    40);
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_SCAN_PASSIVE_TIME,
			    120);

			/* 2. Power management off (PM_OFF = 0) */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_PM, 0);

			/* 3. Roam parameters (brcmf_dongle_roam) */
			(void)cyw_fil_iovar_int_set(sc, "bcn_timeout", 4);
			(void)cyw_fil_iovar_int_set(sc, "roam_off", 1);
			{
				/* [trigger_dBm, band_all] — band 6 = WLC_BAND_ALL */
				uint32_t roam[2];
				roam[0] = htole32((uint32_t)-75);
				roam[1] = htole32(6);
				(void)cyw_fil_cmd_data_set(sc, WLC_SET_ROAM_TRIGGER,
				    roam, sizeof(roam));
				roam[0] = htole32(20);
				roam[1] = htole32(6);
				(void)cyw_fil_cmd_data_set(sc, WLC_SET_ROAM_DELTA,
				    roam, sizeof(roam));
			}

			/* 4. Re-assert STA/infrastructure mode post-WLC_UP.
			 * Mirrors brcmf_cfg80211_change_iface() in Linux which
			 * re-issues WLC_SET_INFRA=1 from brcmf_config_dongle().
			 * This is the step most likely to commit the bsscfg to
			 * its fully-UP state for escan. */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_INFRA, 1);

			/* 5. Frameburst */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_FAKEFRAG, 1);

			/* MPC off so radio stays awake for scan/association */
			(void)cyw_fil_iovar_int_set(sc, "mpc", 0);
			sc->dongle_up = true;
		}
	} else {
		sc->dongle_up = false;
	}
}

/* -------------------------------------------------------------------------
 * ic_raw_xmit stub — FullMAC firmware handles probe requests internally.
 * Returning 0 after freeing silences the "missing ic_raw_xmit callback" log.
 * ------------------------------------------------------------------------- */
static int
cyw_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_bpf_params *params __unused)
{
	ieee80211_free_node(ni);
	m_freem(m);
	return (0);
}

/* -------------------------------------------------------------------------
 * Scan callbacks — enqueue tasks on rx_tq; return immediately.
 *
 * ic_scan_start / ic_scan_end may be called from net80211's own scan
 * taskqueue thread WITHOUT holding the IC lock.  Sleeping IOVARs (escan
 * START / ABORT) must run in process context.  Enqueueing on rx_tq is
 * safe from any context; the task runs when rx_tq is next scheduled.
 * ------------------------------------------------------------------------- */
static void
cyw_scan_start(struct ieee80211com *ic)
{
	struct cyw_softc *sc = ic->ic_softc;

	taskqueue_enqueue(sc->scan_tq, &sc->scan_start_task);
}

static void
cyw_scan_end(struct ieee80211com *ic)
{
	struct cyw_softc *sc = ic->ic_softc;

	taskqueue_enqueue(sc->scan_tq, &sc->scan_end_task);
}

static void
cyw_set_channel(struct ieee80211com *ic __unused)
{
}

/* -------------------------------------------------------------------------
 * Multicast / promisc — no hardware filter programming yet
 * ------------------------------------------------------------------------- */
static void
cyw_update_mcast(struct ieee80211com *ic __unused)
{
}

static void
cyw_update_promisc(struct ieee80211com *ic __unused)
{
}

/* -------------------------------------------------------------------------
 * cyw_cfg_attach — firmware init IOVARs, read MAC, register with net80211
 *
 * Called after cyw_sdpcm_attach (sdpcm_running == true), so IOVARs here
 * use the condvar path in fwil.
 * ------------------------------------------------------------------------- */
int
cyw_cfg_attach(struct cyw_softc *sc)
{
	struct ieee80211com *ic = &sc->ic;
	int err;

	/*
	 * Called from cyw_attach() BEFORE cyw_sdpcm_attach() (sdpcm_running
	 * still false).  IOVARs here use the boot-time polling path in
	 * cyw_fil_txrx().  By the time we arrive, the 5 dongle-init IOVARs
	 * have triggered the SR init sequence (TOHOST=HMB_DATA_FWREADY,
	 * INTSTATUS data-available bits set), so the polling path works on
	 * both fresh-boot and reload-after-kldunload.
	 */
	err = cyw_fil_iovar_data_get(sc, "cur_etheraddr",
	    sc->mac_addr, sizeof(sc->mac_addr));
	if (err != 0) {
		device_printf(sc->dev,
		    "cyw_cfg: cur_etheraddr failed: %d\n", err);
		return (err);
	}
	device_printf(sc->dev, "MAC: %6D\n", sc->mac_addr, ":");

	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->mac_addr);
	ic->ic_softc     = sc;
	ic->ic_name      = device_get_nameunit(sc->dev);
	ic->ic_opmode    = IEEE80211_M_STA;
	ic->ic_phytype   = IEEE80211_T_HT;
	ic->ic_caps      =
	    IEEE80211_C_STA        |	/* infrastructure station mode */
	    IEEE80211_C_IBSS       |	/* ad-hoc mode */
	    IEEE80211_C_MONITOR    |	/* monitor mode */
	    IEEE80211_C_SHPREAMBLE |	/* short preamble */
	    IEEE80211_C_SHSLOT     |	/* short slot time */
	    IEEE80211_C_WPA        |	/* WPA/WPA2 */
	    IEEE80211_C_WME        |	/* QoS / WMM */
	    IEEE80211_C_BGSCAN;		/* background scan */

	/*
	 * ic_getradiocaps is called by ieee80211_ifattach to build the initial
	 * channel list; set it before calling ifattach.
	 */
	ic->ic_getradiocaps = cyw_getradiocaps;

	/* ieee80211_chan_init KASSERTs ic_nchans > 0 without calling
	 * ic_getradiocaps — pre-populate the channel list ourselves. */
	ic->ic_nchans = 0;
	cyw_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans, ic->ic_channels);

	ieee80211_ifattach(ic);

	/* Subscribe to firmware events (event_msgs IOVAR) */
	err = cyw_event_attach(sc);
	if (err != 0) {
		ieee80211_ifdetach(ic);
		return (err);
	}

	/* Register E_ESCAN_RESULT handler (Milestone 2.4) */
	err = cyw_scan_attach(sc);
	if (err != 0) {
		ieee80211_ifdetach(ic);
		return (err);
	}

	/* Override callbacks after ifattach (bwn/iwm convention) */
	ic->ic_vap_create     = cyw_vap_create;
	ic->ic_vap_delete     = cyw_vap_delete;
	ic->ic_transmit       = cyw_transmit;
	ic->ic_update_mcast   = cyw_update_mcast;
	ic->ic_update_promisc = cyw_update_promisc;
	ic->ic_scan_start     = cyw_scan_start;
	ic->ic_scan_end       = cyw_scan_end;
	ic->ic_set_channel    = cyw_set_channel;
	ic->ic_parent         = cyw_parent;
	ic->ic_raw_xmit       = cyw_raw_xmit;

	return (0);
}

void
cyw_cfg_detach(struct cyw_softc *sc)
{
	cyw_scan_detach(sc);
	ieee80211_ifdetach(&sc->ic);
}
