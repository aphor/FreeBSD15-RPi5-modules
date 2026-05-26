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
		 * Park the radio on the target chanspec BEFORE WLC_SET_SSID.
		 *
		 * Observation (2026-05-25): after escan completes the firmware
		 * leaves the radio on the last scanned channel — typically a
		 * 5 GHz chan (36, 42, ...) when scanning all bands.  When we
		 * then issue WLC_SET_SSID with chanspec_list[0] set to the
		 * target chan (e.g. chan 8 / chanspec 0x1008), the firmware
		 * accepts the command (returns 0) but reports
		 * E_SET_SSID status=3 (NO_NETWORKS) because its on-channel
		 * BSS cache for the *current* channel doesn't contain the
		 * target — the chanspec_list hint isn't strong enough to
		 * trigger a channel switch + active probe within the join.
		 *
		 * Explicitly setting "chanspec" first switches the radio
		 * synchronously, after which SET_SSID with a matching
		 * chanspec_list lands on a freshly probable channel.
		 */
		{
			uint16_t chanspec;
			int ieee_chan, cs_err;
			uint32_t cs;

			ieee_chan = (vap->iv_bss != NULL) ?
			    vap->iv_bss->ni_chan->ic_ieee : 0;
			chanspec = cyw_chanspec_for_join(sc, bssid, ieee_chan);

			if (chanspec != 0) {
				cs = htole32(chanspec);
				cs_err = cyw_fil_iovar_data_set(sc, "chanspec",
				    &cs, sizeof(cs));
				device_printf(sc->dev,
				    "AUTH: chanspec set 0x%04x returned %d\n",
				    chanspec, cs_err);
			}
		}

		/*
		 * Pre-join radio-state diagnostics, mirroring freebsd-brcmfmac
		 * /src/cfg.c:308-328.  Read back what the firmware currently
		 * thinks its channel, chanspec, TX power, and regdomain are.
		 *
		 * Interpretation:
		 *   - country.cc / country.abbrev = "US" means the country IOVAR
		 *     SET took (we no longer attempt it, so expect NVRAM default,
		 *     often "WW", "ALL", "00", or similar permissive label).
		 *   - qtxpower = 0 indicates the PHY is muted (regdomain-disabled
		 *     channel or some other restriction).  Non-zero (typical
		 *     values 0x4000..0x6000 = 64-96 dBm/16 = 16-24 dBm EIRP)
		 *     means the firmware is permitted to TX.
		 *   - cur_chan should match the chanspec we are about to join
		 *     (our chan=8 -> cur_chan=8 expected).
		 *   - chanspec lower 8 bits also = channel; full 16 bits encodes
		 *     band + bandwidth + sideband.
		 */
		{
			uint32_t cur_chan = 0, cs = 0, txpwr = 0;
			struct {
				char     cc_abbrev[4];
				uint32_t rev;
				char     cc[4];
			} __packed cspec;
			int gerr;

			(void)cyw_fil_cmd_data_get(sc, WLC_GET_CHANNEL,
			    &cur_chan, sizeof(cur_chan));
			(void)cyw_fil_iovar_data_get(sc, "chanspec",
			    &cs, sizeof(cs));
			(void)cyw_fil_iovar_data_get(sc, "qtxpower",
			    &txpwr, sizeof(txpwr));
			memset(&cspec, 0, sizeof(cspec));
			gerr = cyw_fil_iovar_data_get(sc, "country",
			    &cspec, sizeof(cspec));
			device_printf(sc->dev,
			    "pre-join: cur_chan=%u chanspec=0x%04x "
			    "qtxpower=0x%x country=\"%.*s\"/\"%.*s\" rev=%d "
			    "(get_country=%d)\n",
			    le32toh(cur_chan),
			    le16toh((uint16_t)le32toh(cs)),
			    le32toh(txpwr),
			    (int)sizeof(cspec.cc), cspec.cc,
			    (int)sizeof(cspec.cc_abbrev), cspec.cc_abbrev,
			    (int)le32toh(cspec.rev),
			    gerr);
		}

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

		/*
		 * WLC_SET_WSEC_PMK is intentionally NOT issued.  FW 7.45.265
		 * on CYW43455 does not implement the firmware-supplicant
		 * iovar `sup_wpa` (returns BCME_UNSUPPORTED, see
		 * doc/cyw43455.md §16.8), so the PMK is owned by
		 * wpa_supplicant on the host and the firmware never needs
		 * (or expects) it.  Mirrors Linux brcmfmac, which only
		 * calls brcmf_set_pmk inside `if (use_fwsup != NONE)`
		 * (cfg80211.c:2495-2507).  The host-side PSK sysctl and
		 * cyw_set_pmk() are kept available for future firmware that
		 * does advertise FWSUP — the probe (hw.cyw43455.probe_fwsup)
		 * is the gate.
		 */

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
			chanspec = cyw_chanspec_for_join(sc, bssid, ieee_chan);

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
 * Key management — diagnostic logging only (Step 1 instrumentation for
 * §16.8 Item 4).  None of these actually push the key down to the
 * firmware yet; their purpose right now is to confirm whether
 * wpa_supplicant's IEEE80211_IOC_SETKEY ioctl reaches the driver at
 * all during a 4-way handshake attempt.
 *
 * iv_key_alloc must return a software keyix:
 *   - For group keys (IEEE80211_KEY_GROUP), use the four built-in
 *     iv_nw_keys slots (keyix = k - vap->iv_nw_keys, 0..3).
 *   - For pairwise keys, return slot 0 as a placeholder.
 *
 * iv_key_set/iv_key_delete return 1 (success) so net80211 doesn't tear
 * down the handshake state on a "key install failed" path; the firmware
 * just won't actually encrypt/decrypt yet.  Real WLC_SET_KEY wiring is
 * the next commit if this commit shows wpa_supplicant *is* calling
 * these.
 * ------------------------------------------------------------------------- */
static int
cyw_key_alloc(struct ieee80211vap *vap, struct ieee80211_key *k,
    ieee80211_keyix *keyix, ieee80211_keyix *rxkeyix)
{
	struct cyw_softc *sc = vap->iv_ic->ic_softc;

	if (k->wk_flags & IEEE80211_KEY_GROUP) {
		*keyix = *rxkeyix = (ieee80211_keyix)(k - vap->iv_nw_keys);
	} else {
		*keyix = *rxkeyix = 0;
	}
	device_printf(sc->dev,
	    "iv_key_alloc: cipher=%u flags=0x%x len=%u → keyix=%u rxkeyix=%u\n",
	    k->wk_cipher != NULL ? k->wk_cipher->ic_cipher : 0xff,
	    k->wk_flags, k->wk_keylen, *keyix, *rxkeyix);
	return (1);
}

static int
cyw_key_set(struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	struct cyw_softc *sc = vap->iv_ic->ic_softc;

	device_printf(sc->dev,
	    "iv_key_set: cipher=%u keyix=%u rxkeyix=%u flags=0x%x "
	    "len=%u macaddr=%6D\n",
	    k->wk_cipher != NULL ? k->wk_cipher->ic_cipher : 0xff,
	    k->wk_keyix, k->wk_rxkeyix, k->wk_flags, k->wk_keylen,
	    k->wk_macaddr, ":");
	return (1);	/* TODO: push to firmware via WLC_SET_KEY iovar */
}

static int
cyw_key_delete(struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	struct cyw_softc *sc = vap->iv_ic->ic_softc;

	device_printf(sc->dev,
	    "iv_key_delete: keyix=%u rxkeyix=%u flags=0x%x\n",
	    k->wk_keyix, k->wk_rxkeyix, k->wk_flags);
	return (1);
}

static void
cyw_key_update_begin(struct ieee80211vap *vap)
{
	struct cyw_softc *sc = vap->iv_ic->ic_softc;

	device_printf(sc->dev, "iv_key_update_begin\n");
}

static void
cyw_key_update_end(struct ieee80211vap *vap)
{
	struct cyw_softc *sc = vap->iv_ic->ic_softc;

	device_printf(sc->dev, "iv_key_update_end\n");
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

	/* Diagnostic key-op shims — see comment above the cyw_key_* funcs. */
	vap->iv_key_alloc        = cyw_key_alloc;
	vap->iv_key_set          = cyw_key_set;
	vap->iv_key_delete       = cyw_key_delete;
	vap->iv_key_update_begin = cyw_key_update_begin;
	vap->iv_key_update_end   = cyw_key_update_end;

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
 * Radio capabilities — 2.4 GHz B/G + 5 GHz A (20 MHz primary channels only).
 *
 * The CYW43455 is a single-stream 802.11ac chip with both 2.4 GHz and
 * 5 GHz radios.  HT40/VHT80 widths are not registered with net80211
 * here because net80211's STA join path only ever specifies the
 * primary 20 MHz channel; the firmware uses its scan-result chanspec
 * cache (see cyw_chanspec_for_join() in cyw43455_scan.c) to round-trip
 * the full bandwidth + sideband encoding when issuing the join.
 *
 * 5 GHz channel selection deliberately includes only the non-DFS
 * U-NII-1 and U-NII-3 sub-bands — channels 36/40/44/48 and
 * 149/153/157/161.  DFS channels (52-64, 100-140) require radar
 * detection that the firmware handles but our driver does not yet
 * surface to net80211, so leaving them out avoids spurious channel
 * availability that the firmware would refuse.
 * ------------------------------------------------------------------------- */
static const uint8_t cyw_chans_5ghz_unii1[] =
    { 36, 40, 44, 48 };
static const uint8_t cyw_chans_5ghz_unii3[] =
    { 149, 153, 157, 161 };

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
	ieee80211_add_channel_list_5ghz(chans, maxchans, nchans,
	    cyw_chans_5ghz_unii1, nitems(cyw_chans_5ghz_unii1), bands, 0);
	ieee80211_add_channel_list_5ghz(chans, maxchans, nchans,
	    cyw_chans_5ghz_unii3, nitems(cyw_chans_5ghz_unii3), bands, 0);
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
	struct ether_header *eh;
	uint8_t *bdc, *frame_buf;
	uint8_t *pkt;
	size_t eth_len, framelen;
	uint16_t ethertype;
	bool is_eapol;
	uint8_t priority;
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

	/*
	 * EAPOL classification — mirrors Linux brcmf_netdev_start_xmit
	 * (core.c:353-359).  EAPOL frames (EtherType 0x888E) get classified
	 * via cfg80211_classify8021d() which returns 0 for non-IP traffic;
	 * the Linux BCDC header priority for EAPOL ends up at 0 (the same
	 * value best-effort data gets).  We mirror this — priority 0 — but
	 * log the TX so we can confirm wpa_supplicant's EAPOL M2 / M4 are
	 * actually reaching the firmware.  If tx_eapol_frames stays at 0
	 * during a handshake attempt while rx_eapol_frames > 0, the M2 is
	 * being dropped between wpa_supplicant and us — not in the firmware.
	 */
	eh = mtod(m, struct ether_header *);
	if (eth_len >= sizeof(*eh)) {
		ethertype = ntohs(eh->ether_type);
		is_eapol  = (ethertype == ETHERTYPE_PAE);
	} else {
		ethertype = 0;
		is_eapol  = false;
	}
	priority = 0;	/* matches Linux BCDC priority for non-IP / EAPOL */

	sc->tx_data_frames++;
	if (is_eapol) {
		sc->tx_eapol_frames++;
		sc->tx_eapol_bytes += eth_len;
		device_printf(sc->dev,
		    "TX EAPOL: len=%zu ethertype=0x%04x prio=%u src=%6D dst=%6D\n",
		    eth_len, ethertype, priority,
		    eh->ether_shost, ":", eh->ether_dhost, ":");
	}

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
	bdc[1] = priority;			/* 802.1d priority (0 for EAPOL/BE) */
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

	device_printf(sc->dev,
	    "cyw_parent: nrunning=%d dongle_up=%d\n",
	    ic->ic_nrunning, sc->dongle_up);

	if (ic->ic_nrunning > 0) {
		if (!sc->dongle_up) {
			/*
			 * First-time interface up — equivalent of Linux's
			 * brcmf_config_dongle(), called from ndo_open.
			 *
			 * WLC_UP is issued here, matching Linux exactly: C_UP runs
			 * from brcmf_config_dongle() on first ifconfig up, never
			 * from the attach path.  The 200 ms pause mirrors the Linux
			 * driver's settle window after C_UP.
			 *
			 * The remaining config_dongle commands are commented out
			 * pending confirmation that each is necessary for escan.
			 * Minimising to WLC_UP + mpc=0 while diagnosing BCME_NOTUP.
			 */
			device_printf(sc->dev, "cyw_parent: issuing WLC_UP\n");
			if (cyw_fil_cmd_int_set(sc, WLC_UP, 0) != 0)
				device_printf(sc->dev, "cyw_parent: WLC_UP failed\n");
			else
				device_printf(sc->dev, "cyw_parent: WLC_UP ok\n");
			device_printf(sc->dev, "cyw_parent: pausing 200ms\n");
			pause("cywup", howmany(200 * hz, 1000));
			device_printf(sc->dev, "cyw_parent: pause done\n");

			/*
			 * Step 1 FWSUP probe — must run AFTER WLC_UP, since
			 * sup_wpa returns BCME_NOTUP when the firmware is DOWN
			 * (verified at attach: error -23 / errno 5).
			 */
			if (cyw_probe_fwsup_tunable)
				cyw_probe_fwsup(sc);

			/* WLC_SET_INFRA=1 — commit BSS to STA/infrastructure mode.
			 * Linux issues this from brcmf_cfg80211_change_iface() inside
			 * brcmf_config_dongle(), always after C_UP.  This ordering
			 * (UP then SET_INFRA) has not previously been tested. */
			if (cyw_fil_cmd_int_set(sc, WLC_SET_INFRA, 1) != 0)
				device_printf(sc->dev,
				    "cyw_parent: WLC_SET_INFRA failed\n");
			else
				device_printf(sc->dev,
				    "cyw_parent: WLC_SET_INFRA ok\n");

			/*
			 * bss enable=1 removed.  freebsd-brcmfmac does not issue
			 * it on the primary STA bsscfg; our previous "successful"
			 * call relied on a misformatted payload (4-zero prefix
			 * from the broken bsscfg_int_set) that accidentally
			 * matched the firmware's required [bsscfgidx_le32]
			 * [enable_le32] format.  With bsscfg_int_set fixed it
			 * would now BCME_NOTUP, and per freebsd-brcmfmac the call
			 * is not needed here at all.
			 */

			/*
			 * Preinit IOVARs after WLC_UP — mirror freebsd-brcmfmac
			 * /src/cfg.c:1120-1153.  These were previously deferred
			 * pending evidence; that evidence now exists in the form
			 * of the working freebsd-brcmfmac driver on the same chip.
			 * All non-fatal — firmware may ignore some on 7.45.x.
			 */
			(void)cyw_fil_cmd_int_set(sc,
			    WLC_SET_SCAN_CHANNEL_TIME, 40);
			(void)cyw_fil_cmd_int_set(sc,
			    WLC_SET_SCAN_UNASSOC_TIME, 40);
			(void)cyw_fil_cmd_int_set(sc,
			    WLC_SET_SCAN_PASSIVE_TIME, 120);
			(void)cyw_fil_iovar_int_set(sc, "bcn_timeout", 4);
			(void)cyw_fil_iovar_int_set(sc, "assoc_retry_max", 3);
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_FAKEFRAG, 1);
			(void)cyw_fil_iovar_int_set(sc, "txbf", 1);

			/*
			 * QUESTION: Does WLC_SET_PM here conflict with the pm=0
			 * IOVAR issued during attach (boot-time polling path)?
			 * WLC_SET_PM (cmd 86) and the "pm" IOVAR (cmd 263) address
			 * the same firmware power-management knob.  Issuing both may
			 * be redundant; "pm" IOVAR returns BCME_UNSUPPORTED on this
			 * firmware, so WLC_SET_PM may be the correct form — but it
			 * is unclear whether sending it here (post-WLC_UP) is needed
			 * or whether the attach-time "pm" IOVAR attempt suffices.
			 */
#if 0	/* PM off — correct command form unclear; may belong in attach */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_PM, 0);
#endif

			/*
			 * QUESTION: Are roam parameters needed before escan works?
			 * These are association-phase tuning; no evidence they gate
			 * scan readiness.  Additionally, WLC_SET_ROAM_TRIGGER (55)
			 * and WLC_SET_ROAM_DELTA (57) both return BCME_BADARG with
			 * the current uint32_t[2] payload — the correct wire format
			 * for firmware 7.45.265 has not been verified against the
			 * Linux brcmf_roam_trigger_le struct definition.
			 */
#if 0	/* roam params — BCME_BADARG; format unverified; not scan-critical */
			(void)cyw_fil_iovar_int_set(sc, "bcn_timeout", 4);
			(void)cyw_fil_iovar_int_set(sc, "roam_off", 1);
			{
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
#endif

			/*
			 * QUESTION: Does re-issuing WLC_SET_INFRA=1 after WLC_UP
			 * reset the BSS to a non-UP state?  Linux re-issues it from
			 * brcmf_cfg80211_change_iface() called inside
			 * brcmf_config_dongle(), but WLC_SET_INFRA may implicitly
			 * trigger a WLC_DOWN internally in the firmware, undoing
			 * WLC_UP.  This is the most likely candidate for causing
			 * BCME_NOTUP on subsequent escan.
			 */
#if 0	/* SET_INFRA re-assert — may reset BSS UP state; investigate first */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_INFRA, 1);
#endif

			/*
			 * QUESTION: Is WLC_SET_FAKEFRAG (frameburst) needed before
			 * scanning?  This is a TX throughput optimisation with no
			 * known relationship to scan readiness.
			 */
#if 0	/* frameburst — TX optimisation; no evidence needed for scan */
			(void)cyw_fil_cmd_int_set(sc, WLC_SET_FAKEFRAG, 1);
#endif

			/*
			 * MPC (Minimum Power Consumption) is NOT disabled here.
			 * Linux sets mpc=1 at preinit and never disables it for
			 * scanning on the CYW43455 (chip 0x4345).  The NEED_MPC
			 * quirk that drives mpc=0-before-scan in brcmf_do_escan
			 * is only assigned to chip 0x4329 (BCM4329, circa 2009).
			 * mpc=0 at attach time is also removed; firmware default
			 * (mpc=1) is left in place.
			 */
			sc->dongle_up = true;
			device_printf(sc->dev, "cyw_parent: dongle_up set\n");
		}
	} else {
		sc->dongle_up = false;
	}
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
 * ic_raw_xmit — instrumented to confirm whether wpa_supplicant's EAPOL
 * M2 is being routed through here instead of ic_transmit.
 *
 * Previously a stub that silently dropped every frame.  If this fires
 * during a 4-way handshake while tx_data_frames stays at 0, we've found
 * the EAPOL TX leak: the BPF / l2_packet path FreeBSD's wpa_supplicant
 * (-Dbsd) uses lands in ic_raw_xmit, not ic_transmit, and we drop it.
 * ------------------------------------------------------------------------- */
static int
cyw_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_bpf_params *params __unused)
{
	struct cyw_softc *sc = ni->ni_ic->ic_softc;
	int len = (m != NULL) ? m->m_pkthdr.len : -1;
	uint8_t first[16] = { 0 };
	int copylen = (m != NULL && m->m_pkthdr.len > 0) ?
	    MIN(m->m_pkthdr.len, (int)sizeof(first)) : 0;

	if (copylen > 0)
		m_copydata(m, 0, copylen, first);

	device_printf(sc->dev,
	    "raw_xmit: len=%d first16=%02x%02x%02x%02x%02x%02x%02x%02x"
	    "%02x%02x%02x%02x%02x%02x%02x%02x\n", len,
	    first[0], first[1], first[2], first[3],
	    first[4], first[5], first[6], first[7],
	    first[8], first[9], first[10], first[11],
	    first[12], first[13], first[14], first[15]);

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
