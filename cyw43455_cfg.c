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
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sx.h>
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
 * cyw_newstate — drive the firmware association state machine.
 *
 * Net80211 calls this with IC lock held.  IOVARs sleep, so we drop the IC
 * lock around them and re-acquire before chaining to the default handler.
 *
 * State transitions handled here:
 *   * → INIT:   if link_up, issue WLC_DISASSOC.
 *   * → AUTH:   abort any escan, push wsec/wpa_auth/wsec_pmk, issue
 *               WLC_SET_SSID with the target BSSID.  Firmware drives
 *               802.11 auth + assoc + 4-way handshake internally and
 *               reports completion via E_LINK (handled by security.c).
 *
 * FullMAC behaviour: net80211's iv_mgtsend callout fires after 2s and
 * aborts AUTH/ASSOC if the host hasn't seen mgmt frames.  Since firmware
 * never surfaces those frames to the host, we cancel that callout for
 * AUTH and ASSOC (mirrors brcmf_newstate in freebsd-brcmfmac).
 *
 * Reference: freebsd-brcmfmac/src/cfg.c brcmf_newstate
 * ------------------------------------------------------------------------- */
static int
cyw_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	struct cyw_vap *cvap = (struct cyw_vap *)vap;
	struct ieee80211com *ic = vap->iv_ic;
	struct cyw_softc *sc = ic->ic_softc;
	static const char *state_names[] = {
		"INIT", "SCAN", "AUTH", "ASSOC", "CAC", "RUN", "CSA", "SLEEP"
	};
	int ret;

	device_printf(sc->dev, "newstate: %s -> %s arg=%d\n",
	    state_names[vap->iv_state], state_names[nstate], arg);

	IEEE80211_UNLOCK(ic);

	switch (nstate) {
	case IEEE80211_S_INIT:
		if (sc->link_up) {
			struct {
				uint32_t val;
				uint8_t  ea[6];
				uint8_t  pad[2];
			} scbval;
			memset(&scbval, 0, sizeof(scbval));
			scbval.val = htole32(3);	/* DEAUTH_LEAVING */
			memcpy(scbval.ea, sc->join_bssid, 6);
			(void)cyw_fil_cmd_data_set(sc, WLC_DISASSOC,
			    &scbval, sizeof(scbval));
			sc->link_up = false;
		}
		break;

	case IEEE80211_S_AUTH: {
		struct ieee80211_node *ni;
		struct ieee80211_channel *chan;
		uint8_t  bssid[6];
		uint8_t  essid[IEEE80211_NWID_LEN];
		uint8_t  esslen;
		uint32_t wsec     = CYW_WSEC_NONE;
		uint32_t wpa_auth = CYW_WPA_AUTH_DISABLED;
		uint16_t psk_len;
		uint8_t  psk[CYW_WSEC_MAX_PSK_LEN];
		int err;

		/* abort any in-flight escan so firmware can switch channel */
		cyw_abort_escan(sc);
		pause("cywab", howmany(100 * hz, 1000));

		IEEE80211_LOCK(ic);
		ni = vap->iv_bss;
		if (ni == NULL) {
			IEEE80211_UNLOCK(ic);
			device_printf(sc->dev, "AUTH: no iv_bss; ignoring\n");
			break;
		}
		IEEE80211_ADDR_COPY(bssid, ni->ni_bssid);
		chan   = ni->ni_chan;
		esslen = ni->ni_esslen;
		memcpy(essid, ni->ni_essid, esslen);
		IEEE80211_UNLOCK(ic);

		if (vap->iv_flags & IEEE80211_F_WPA2) {
			wsec     = CYW_AES_ENABLED;
			wpa_auth = CYW_WPA2_AUTH_PSK;
		} else if (vap->iv_flags & IEEE80211_F_WPA1) {
			wsec     = CYW_TKIP_ENABLED;
			wpa_auth = CYW_WPA_AUTH_PSK;
		} else if (vap->iv_flags & IEEE80211_F_PRIVACY) {
			wsec     = CYW_WEP_ENABLED;
		}

		device_printf(sc->dev,
		    "AUTH: bssid=%6D ssid=\"%.*s\" iv_flags=0x%x wsec=0x%x "
		    "wpa_auth=0x%x\n",
		    bssid, ":", esslen, essid, vap->iv_flags, wsec, wpa_auth);

		/*
		 * Do NOT send the "wpaie" iovar on CYW43455.
		 *
		 * freebsd-brcmfmac/src/cfg.c:597 documents: "CYW43455 (7.45.x)
		 * returns UNSUPPORTED for wpaie, and the failed iovar can
		 * taint firmware WPA state".  cfg.c:1018: "BCM4350 (7.35.x)
		 * accepts it but sending the RSN IE corrupts firmware WPA
		 * state, causing AUTH frames to fail with NO_ACK. CYW43455
		 * (7.45.x) returns BCME_UNSUPPORTED. Do not enable on any
		 * chip."  Even when the iovar appears to succeed (returns 0),
		 * the firmware's internal WPA state can be silently poisoned.
		 * Our earlier experimental wpaie call has been removed.
		 */

		err = cyw_set_security(sc, wsec, wpa_auth);
		if (err != 0) {
			device_printf(sc->dev, "AUTH: set_security failed: %d\n",
			    err);
		}

		/* Push PSK if WPA-class network and a passphrase is stored. */
		if (wpa_auth != CYW_WPA_AUTH_DISABLED) {
			CYW_LOCK(sc);
			psk_len = sc->psk_len;
			memcpy(psk, sc->psk, sizeof(psk));
			CYW_UNLOCK(sc);

			if (psk_len == 0) {
				device_printf(sc->dev,
				    "AUTH: WPA network but no PSK set "
				    "(use sysctl hw.cyw43455.psk)\n");
			} else {
				(void)cyw_set_pmk(sc, psk, psk_len);
			}
		}

		memcpy(sc->join_bssid, bssid, 6);

		/*
		 * Extended "join" IOVAR — primary path in Linux brcmfmac
		 * (see brcmf_cfg80211_connect, cfg80211.c:2368-2605, and
		 * struct brcmf_ext_join_params_le in fwil_types.h:534-539).
		 *
		 * The embedded scan_le block tells firmware to actively probe
		 * for SSID/BSSID on the supplied chanspec _during_ the join,
		 * so the firmware's internal BSS cache no longer has to be
		 * hot.  This fixes the E_SET_SSID status=3 (NO_NETWORKS) we
		 * were getting from WLC_SET_SSID after wpa_supplicant-driven
		 * scans — see doc/cyw43455.md §16.4 for the full analysis.
		 *
		 * scan_le fields are set to -1 sentinels, the documented
		 * "use firmware defaults" values (Linux cfg80211.c:2536-2542).
		 *
		 * CYW43455 firmware uses D11AC chanspec encoding (verified by
		 * observing 0x1008 in scan results for 2.4GHz ch 8 / 20MHz):
		 *   bits[15:14] band: 2G=0x0000 / 5G=0xC000
		 *   bits[12:11] bw:   20MHz=0x1000
		 *   bits[10:8]  sb:   LLL=0x0000 (primary lower for 20MHz)
		 *   bits[7:0]   IEEE channel number
		 */
		{
			struct cyw_ext_join_params join;
			uint16_t chanspec;
			int ieee_chan;

			ieee_chan = (chan != NULL) ? chan->ic_ieee : 0;
			if (ieee_chan == 0) {
				chanspec = 0;
			} else if (ieee_chan <= 14) {
				chanspec = 0x1000 | (uint16_t)ieee_chan;
			} else {
				chanspec = 0xD000 | (uint16_t)ieee_chan;
			}

			memset(&join, 0, sizeof(join));
			join.ssid_le.SSID_len = htole32(esslen);
			memcpy(join.ssid_le.SSID, essid, esslen);

			/*
			 * Mirror Linux brcmf_cfg80211_connect scan_le init
			 * (cfg80211.c:2543-2572).  When a channel is known
			 * (chanspec != 0), use BRCMF_SCAN_JOIN_* dwell times
			 * — these are documented to be needed at noisy air
			 * to receive a probe response or beacon from the
			 * target AP during the join.  Without channel known,
			 * everything stays at the -1 sentinel (firmware
			 * defaults).  scan_type is -1 (0xff) per Linux, not 0.
			 */
			join.scan_le.scan_type    = (uint8_t)-1;
			join.scan_le.home_time    = (int32_t)htole32((uint32_t)-1);
			if (chanspec != 0) {
				/* 320 ms active, 400 ms passive, nprobes=16 */
				join.scan_le.active_time  =
				    (int32_t)htole32(320);
				join.scan_le.passive_time =
				    (int32_t)htole32(400);
				join.scan_le.nprobes      =
				    (int32_t)htole32(16);
			} else {
				join.scan_le.nprobes      =
				    (int32_t)htole32((uint32_t)-1);
				join.scan_le.active_time  =
				    (int32_t)htole32((uint32_t)-1);
				join.scan_le.passive_time =
				    (int32_t)htole32((uint32_t)-1);
			}

			memcpy(join.assoc_le.bssid, bssid, 6);
			if (chanspec != 0) {
				join.assoc_le.chanspec_num = htole32(1);
				join.assoc_le.chanspec_list[0] = htole16(chanspec);
			} else {
				join.assoc_le.chanspec_num = htole32(0);
			}

			/*
			 * Use bsscfg-scoped iovar: prepends a 4-byte LE
			 * bsscfg index (= 0, primary BSS) matching Linux
			 * brcmf_fil_bsscfg_data_set().  Without the prefix,
			 * firmware interprets the first 4 bytes of
			 * ext_join_params (SSID_len field) as the bsscfg
			 * index, looks up a non-existent BSS, and returns
			 * BCME_NOTUP (-14).
			 */
			err = cyw_fil_bsscfg_data_set(sc, "join",
			    &join, sizeof(join));
			device_printf(sc->dev,
			    "AUTH: join chan=%d chanspec=0x%04x returned %d\n",
			    ieee_chan, chanspec, err);

			/*
			 * Fallback: WLC_SET_SSID legacy join.
			 *
			 * Linux brcmf_cfg80211_connect (cfg80211.c:2587-2601)
			 * falls back to BRCMF_C_SET_SSID with brcmf_join_params
			 * when the extended "join" IOVAR fails.  CYW43455
			 * firmware 7.45.265 appears to selectively reject the
			 * bsscfg-scoped "join" with a stable BCME_NOTUP even
			 * with bss enable=1, wpaie, set_pmk, and bsscfg-scoped
			 * security iovars all confirmed succeeding.  Now that
			 * those prerequisites are in place, WLC_SET_SSID may
			 * succeed where it previously returned NO_NETWORKS.
			 *
			 * Format: brcmf_join_params = ssid_le + assoc_params_le
			 * (see cyw_join_params in cyw43455_var.h).  Sent as a
			 * raw cmd (not iovar), matching Linux's
			 * brcmf_fil_cmd_data_set(ifp, BRCMF_C_SET_SSID, ...).
			 */
			if (err != 0) {
				struct cyw_join_params jp;
				int set_err;

				memset(&jp, 0, sizeof(jp));
				jp.ssid_le.SSID_len = htole32(esslen);
				memcpy(jp.ssid_le.SSID, essid, esslen);
				memcpy(jp.params_le.bssid, bssid, 6);
				if (chanspec != 0) {
					jp.params_le.chanspec_num =
					    htole32(1);
					jp.params_le.chanspec_list[0] =
					    htole16(chanspec);
				} else {
					jp.params_le.chanspec_num =
					    htole32(0);
				}
				set_err = cyw_fil_cmd_data_set(sc,
				    WLC_SET_SSID, &jp, sizeof(jp));
				device_printf(sc->dev,
				    "AUTH: WLC_SET_SSID fallback (chan=%d) "
				    "returned %d\n", ieee_chan, set_err);
				if (set_err == 0)
					err = 0;
			}
		}
		break;
	}

	default:
		break;
	}

	IEEE80211_LOCK(ic);
	ret = cvap->cv_newstate(vap, nstate, arg);

	/*
	 * FullMAC: firmware handles auth/assoc; cancel net80211's mgmt-frame
	 * timeout that would otherwise abort AUTH/ASSOC after 2s.
	 */
	if (nstate == IEEE80211_S_AUTH || nstate == IEEE80211_S_ASSOC)
		callout_stop(&vap->iv_mgtsend);

	return (ret);
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
 * Transmit — build [SDPCM hdr | BDC data hdr | 802.3 frame] and write to F2.
 *
 * The 4-byte BDC data header that precedes Ethernet frames on CHAN_DATA is
 * distinct from the 16-byte BCDC command header used on CHAN_CTRL.  Format:
 *   [0] flags  = (BCDC_PROTO_VER=2) << 4
 *   [1] priority (802.1d, 0 = best-effort)
 *   [2] flags2 (interface index, 0 for single-BSS STA)
 *   [3] data_offset = 0 (no padding between BDC hdr and frame)
 *
 * Credits are checked against sdpcm_rx_max (updated from every RX header).
 * Frames dropped when no credits are available rather than blocking, since
 * ic_transmit may be called from contexts that limit sleep duration.
 *
 * Reference: Linux brcmfmac bcdc.c brcmf_proto_bcdc_hdrpush().
 * ------------------------------------------------------------------------- */
#define ALIGN4(x)		(((x) + 3) & ~3)
#define CYW_BDC_DATA_HDR_LEN	4
#define CYW_BCDC_PROTO_VER	2

static int
cyw_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	struct cyw_softc *sc = ic->ic_softc;
	struct cyw_sdpcm_hdr *sph;
	uint8_t *bdc, *frame_buf;
	uint8_t *pkt;
	size_t eth_len, framelen;
	int err;

	/* Linearize — mbuf chain from net80211 may be scattered. */
	if (m->m_next != NULL) {
		struct mbuf *n = m_pullup(m, m->m_pkthdr.len);
		if (n == NULL)
			return (ENOBUFS);
		m = n;
	}
	eth_len   = m->m_len;
	framelen  = ALIGN4(CYW_SDPCM_HDR_LEN + CYW_BDC_DATA_HDR_LEN + eth_len);

	/* Drop rather than block if no TX credits. */
	if ((uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq) == 0) {
		m_freem(m);
		return (ENOBUFS);
	}

	pkt = malloc(framelen, M_CYW43455, M_NOWAIT | M_ZERO);
	if (pkt == NULL) {
		m_freem(m);
		return (ENOBUFS);
	}

	sph = (struct cyw_sdpcm_hdr *)pkt;
	sph->len        = htole16((uint16_t)framelen);
	sph->len_inv    = htole16(~(uint16_t)framelen);
	sph->chan_flags  = CYW_SDPCM_CHAN_DATA;
	sph->data_offset = CYW_SDPCM_HDR_LEN;

	bdc    = pkt + CYW_SDPCM_HDR_LEN;
	bdc[0] = (CYW_BCDC_PROTO_VER << 4);	/* flags: proto ver */
	bdc[1] = 0;				/* priority */
	bdc[2] = 0;				/* flags2 / ifidx */
	bdc[3] = 0;				/* data_offset: no padding */

	frame_buf = pkt + CYW_SDPCM_HDR_LEN + CYW_BDC_DATA_HDR_LEN;
	memcpy(frame_buf, mtod(m, void *), eth_len);
	m_freem(m);

	sx_xlock(&sc->f2_sx);
	sph->seq = sc->sdpcm_tx_seq++;
	err = cyw_f2_write_block(sc, pkt, framelen);
	sx_xunlock(&sc->f2_sx);

	free(pkt, M_CYW43455);
	return (err);
}

/* -------------------------------------------------------------------------
 * Interface parent — tracks ic_nrunning transitions.
 *
 * The full firmware bring-up sequence (WLC_DOWN → btc_mode → SET_INFRA →
 * roam tuning → WLC_UP → 200 ms settle → preinit IOVARs) now lives in
 * cyw_attach, mirroring freebsd-brcmfmac/src/cfg.c:1025-1153 which is the
 * only known-working sequence for CYW43455 firmware on FreeBSD.  Linux's
 * brcmfmac defers WLC_UP to brcmf_config_dongle (first ifconfig up); that
 * ordering does not work on the SDIO/MMCCAM/BCM2712 path.
 *
 * cyw_parent therefore only needs to track ic_nrunning for diagnostic
 * logging.  All real firmware setup has already happened by the time
 * the interface is first brought up.
 *
 * Called by net80211 via ic_parent_task (taskqueue_thread), without IC lock.
 * ------------------------------------------------------------------------- */
static void
cyw_parent(struct ieee80211com *ic)
{
	struct cyw_softc *sc = ic->ic_softc;

	device_printf(sc->dev,
	    "cyw_parent: nrunning=%d dongle_up=%d\n",
	    ic->ic_nrunning, sc->dongle_up);

	sc->dongle_up = (ic->ic_nrunning > 0);
}

/* -------------------------------------------------------------------------
 * ic_wme.wme_update stub — WME parameters are managed by the FullMAC
 * firmware internally.  net80211 calls wme_update (via vap_update_wme on
 * iv_wme_task) whenever beacon WME IEs change.  Without this stub the call
 * through a null function pointer panics at vap_update_wme+0x40.
 * ------------------------------------------------------------------------- */
static int
cyw_wme_update(struct ieee80211com *ic __unused)
{
	return (0);
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

	device_printf(sc->dev,
	    "cyw_scan_start: dongle_up=%d\n", sc->dongle_up);
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

	/* Step 5: PSK sysctl + E_LINK / E_SET_SSID handlers */
	cyw_security_sysctl_init(sc);
	err = cyw_security_event_attach(sc);
	if (err != 0) {
		cyw_scan_detach(sc);
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
	ic->ic_wme.wme_update = cyw_wme_update;

	return (0);
}

void
cyw_cfg_detach(struct cyw_softc *sc)
{
	cyw_security_event_detach(sc);
	cyw_scan_detach(sc);
	ieee80211_ifdetach(&sc->ic);
}
