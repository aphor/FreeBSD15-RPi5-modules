/*
 * cyw43455_cfg.c — net80211 integration layer
 *
 * Reads the chip MAC address, attaches to net80211, and provides the
 * minimum set of ic callbacks.  This milestone wires the driver into the
 * net80211 framework so the interface appears in ifconfig output.
 *
 * Milestones for the remaining callbacks:
 *   scan_start / scan_end   — Milestone 2.3 (escan)
 *   iv_newstate association — Milestone 2.4/2.5
 *   ic_transmit data path   — Milestone 2.6
 *
 * Reference: /usr/src/sys/net80211/ieee80211.c, if_bwn.c, if_rum.c
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_regdomain.h>
#include <net80211/ieee80211_phy.h>

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
 * State machine
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
 * Radio capability report — 2.4 GHz B/G and 5 GHz A channels.
 * HT/VHT extensions are added in a later milestone.
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

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11A);
	ieee80211_add_channels_default_5ghz(chans, maxchans, nchans, bands, 0);
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
 * Interface parent (up/down) — Milestone 2.4 will issue WLC_UP/DOWN here
 * ------------------------------------------------------------------------- */
static void
cyw_parent(struct ieee80211com *ic)
{
}

/* -------------------------------------------------------------------------
 * Scan stubs — Milestone 2.3 will implement escan
 * ------------------------------------------------------------------------- */
static void
cyw_scan_start(struct ieee80211com *ic __unused)
{
}

static void
cyw_scan_end(struct ieee80211com *ic __unused)
{
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
 * cyw_cfg_attach — read MAC, register with net80211
 *
 * Called after cyw_sdpcm_attach (sdpcm_running == true), so IOVARs here
 * use the condvar path in fwil.
 * ------------------------------------------------------------------------- */
int
cyw_cfg_attach(struct cyw_softc *sc)
{
	struct ieee80211com *ic = &sc->ic;
	int err;

	err = cyw_fil_iovar_data_get(sc, "cur_etheraddr",
	    sc->mac_addr, sizeof(sc->mac_addr));
	if (err != 0) {
		device_printf(sc->dev,
		    "cyw_cfg: cur_etheraddr failed: %d\n", err);
		return (err);
	}
	device_printf(sc->dev, "MAC: %6D\n", sc->mac_addr, ":");

	ic->ic_softc        = sc;
	ic->ic_name         = device_get_nameunit(sc->dev);
	ic->ic_opmode       = IEEE80211_M_STA;
	ic->ic_phytype      = IEEE80211_T_HT;
	ic->ic_caps         =
	    IEEE80211_C_STA        |	/* infrastructure station mode */
	    IEEE80211_C_IBSS       |	/* ad-hoc mode */
	    IEEE80211_C_MONITOR    |	/* monitor mode */
	    IEEE80211_C_SHPREAMBLE |	/* short preamble */
	    IEEE80211_C_SHSLOT     |	/* short slot time */
	    IEEE80211_C_WPA        |	/* WPA/WPA2 */
	    IEEE80211_C_WME        |	/* QoS / WMM */
	    IEEE80211_C_BGSCAN;		/* background scan */

	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	ic->ic_sup_rates[IEEE80211_MODE_11A] = ieee80211_std_rateset_11a;

	ic->ic_vap_create    = cyw_vap_create;
	ic->ic_vap_delete    = cyw_vap_delete;
	ic->ic_transmit      = cyw_transmit;
	ic->ic_update_mcast  = cyw_update_mcast;
	ic->ic_update_promisc = cyw_update_promisc;
	ic->ic_scan_start    = cyw_scan_start;
	ic->ic_scan_end      = cyw_scan_end;
	ic->ic_set_channel   = cyw_set_channel;
	ic->ic_getradiocaps  = cyw_getradiocaps;
	ic->ic_parent        = cyw_parent;

	ieee80211_ifattach(ic, sc->mac_addr);
	return (0);
}

void
cyw_cfg_detach(struct cyw_softc *sc)
{
	ieee80211_ifdetach(&sc->ic);
}
