/*
 * cyw43455_security.c — WPA2-PSK security & E_LINK/E_SET_SSID handlers (Step 5)
 *
 * Provides:
 *   cyw_set_security()     — WLC_SET_AUTH=0, wsec, wpa_auth IOVARs
 *   cyw_set_pmk()          — WLC_SET_WSEC_PMK with raw passphrase
 *   cyw_security_sysctl_init() — hw.cyw43455.psk write-only sysctl
 *   cyw_security_event_attach()  — register E_LINK / E_SET_SSID handlers
 *
 * Reference: freebsd-brcmfmac/src/security.c and Linux brcmfmac cfg80211.c
 *
 * Notes for CYW43455 firmware 7.45.x (per TODO.md and reference notes):
 *   - sup_wpa=1  → BCME_BADARG  (do NOT set internal supplicant)
 *   - WPA2_AUTH_PSK_SHA256 (0x8000) → unsupported, do not include
 *   - WLC_SET_AUTH=0 (open) is required; firmware handles WPA2 4-way
 *     handshake internally given wsec_pmk.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>

#include "cyw43455_var.h"

/* -------------------------------------------------------------------------
 * cyw_set_security — push WLC_SET_AUTH=0, wsec, wpa_auth
 *
 * Called from cyw_newstate on IEEE80211_S_AUTH entry.  No IC lock held.
 * ------------------------------------------------------------------------- */
int
cyw_set_security(struct cyw_softc *sc, uint32_t wsec, uint32_t wpa_auth)
{
	int err;

	/*
	 * Linux brcmf_set_auth_type / brcmf_set_wsec_mode / brcmf_set_key_mgmt
	 * use bsscfg-scoped iovars ("auth", "wsec", "wpa_auth") so the firmware
	 * applies the values to bsscfg 0 — the same bsscfg the subsequent
	 * "join" IOVAR targets.  Issuing these without the 4-byte bsscfg index
	 * prefix may set the wrong bsscfg's security state and leave bsscfg 0
	 * unconfigured, which the firmware can report later as BCME_NOTUP.
	 */
	err = cyw_fil_bsscfg_int_set(sc, "auth", 0); /* 0 = open system */
	if (err != 0) {
		device_printf(sc->dev, "set_security: auth=0 failed: %d\n",
		    err);
		return (err);
	}

	err = cyw_fil_bsscfg_int_set(sc, "wsec", wsec);
	if (err != 0) {
		device_printf(sc->dev, "set_security: wsec=%u failed: %d\n",
		    wsec, err);
		return (err);
	}

	err = cyw_fil_bsscfg_int_set(sc, "wpa_auth", wpa_auth);
	if (err != 0) {
		device_printf(sc->dev, "set_security: wpa_auth=0x%x failed: %d\n",
		    wpa_auth, err);
		return (err);
	}

	device_printf(sc->dev, "security: wsec=0x%x wpa_auth=0x%x\n",
	    wsec, wpa_auth);
	return (0);
}

/* -------------------------------------------------------------------------
 * cyw_set_pmk — push WPA passphrase via WLC_SET_WSEC_PMK
 *
 * Reference: brcmf_set_wsec() in Linux brcmfmac cfg80211.c.
 * Sends a fixed 68-byte payload (struct cyw_wsec_pmk_le); key_len is the
 * actual passphrase length, flags=0 for raw 32-byte PMK or
 * CYW_WSEC_PASSPHRASE for an ASCII passphrase (we use passphrase mode).
 * ------------------------------------------------------------------------- */
int
cyw_set_pmk(struct cyw_softc *sc, const uint8_t *psk, uint16_t len)
{
	struct cyw_wsec_pmk_le pmk;
	int err;

	if (len > CYW_WSEC_MAX_PSK_LEN)
		return (EINVAL);

	memset(&pmk, 0, sizeof(pmk));
	pmk.key_len = htole16(len);
	/*
	 * flags=CYW_WSEC_PASSPHRASE (1) tells the firmware to treat key[] as
	 * an ASCII passphrase and run PBKDF2-SHA1 internally to derive the
	 * 32-byte PMK.  This is required when we pass a raw passphrase (8–63
	 * bytes) rather than a pre-computed PMK.
	 *
	 * Note: Linux brcmf_set_pmk always passes a 32-byte PBKDF2-derived
	 * PMK from wpa_supplicant with flags=0 ("raw PMK").  The comment in
	 * a previous revision that said "match Linux behaviour" was wrong —
	 * Linux's key_len is always 32; ours is the passphrase length.
	 * Firmware rejects key_len != 32 with flags=0 via BCME_BADARG.
	 */
	pmk.flags = htole16(CYW_WSEC_PASSPHRASE);
	if (len > 0)
		memcpy(pmk.key, psk, len);

	err = cyw_fil_cmd_data_set(sc, WLC_SET_WSEC_PMK, &pmk, sizeof(pmk));
	if (err != 0)
		device_printf(sc->dev, "set_pmk: WLC_SET_WSEC_PMK len=%u failed: %d\n",
		    len, err);
	else
		device_printf(sc->dev, "set_pmk: len=%u ok\n", len);
	return (err);
}

/* -------------------------------------------------------------------------
 * psk sysctl — write-only.  Accepts 8–63 char passphrase.
 * ------------------------------------------------------------------------- */
static int
cyw_sysctl_psk(SYSCTL_HANDLER_ARGS)
{
	struct cyw_softc *sc = arg1;
	char buf[CYW_WSEC_MAX_PSK_LEN + 1];
	int err, len;

	memset(buf, 0, sizeof(buf));

	err = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	len = strlen(buf);
	if (len < 8 || len > 63) {
		device_printf(sc->dev, "psk: must be 8-63 characters\n");
		return (EINVAL);
	}

	CYW_LOCK(sc);
	memset(sc->psk, 0, sizeof(sc->psk));
	memcpy(sc->psk, buf, len);
	sc->psk_len = (uint16_t)len;
	CYW_UNLOCK(sc);

	device_printf(sc->dev, "psk: stored %d-byte passphrase\n", len);
	return (0);
}

void
cyw_security_sysctl_init(struct cyw_softc *sc)
{
	if (sc->sysctl_tree == NULL)
		return;
	SYSCTL_ADD_PROC(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO, "psk",
	    CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_MPSAFE, sc, 0,
	    cyw_sysctl_psk, "A", "WPA2 PSK passphrase (write-only, 8-63 chars)");
}

/* -------------------------------------------------------------------------
 * E_LINK / E_SET_SSID event handlers — drive net80211 state transitions
 *
 * Run from cyw_sdpcm_task (rx_tq) in process context, no locks held.
 * ieee80211_new_state is safe from here; it enqueues the transition on
 * net80211's own state task and returns.
 * ------------------------------------------------------------------------- */
static void
cyw_link_event(struct cyw_softc *sc, const struct cyw_event_msg *msg,
    const void *data __unused, size_t datalen __unused)
{
	struct ieee80211com *ic = &sc->ic;
	struct ieee80211vap *vap;
	bool link;

	link = (msg->flags & CYW_EVENT_MSG_LINK) != 0;

	device_printf(sc->dev, "E_LINK: link=%d status=%u reason=%u\n",
	    link, msg->status, msg->reason);

	CYW_LOCK(sc);
	sc->link_up = link;
	CYW_UNLOCK(sc);

	vap = TAILQ_FIRST(&ic->ic_vaps);
	if (vap == NULL)
		return;

	if (link) {
		/* Firmware finished AUTH+ASSOC+4-way → drive VAP to RUN. */
		ieee80211_new_state(vap, IEEE80211_S_RUN, 0);
	} else {
		/* Link went down — return to SCAN so net80211 can re-search. */
		if (vap->iv_state == IEEE80211_S_RUN ||
		    vap->iv_state == IEEE80211_S_AUTH ||
		    vap->iv_state == IEEE80211_S_ASSOC)
			ieee80211_new_state(vap, IEEE80211_S_SCAN, 0);
	}
}

static void
cyw_set_ssid_event(struct cyw_softc *sc, const struct cyw_event_msg *msg,
    const void *data __unused, size_t datalen __unused)
{
	struct ieee80211com *ic = &sc->ic;
	struct ieee80211vap *vap;

	device_printf(sc->dev, "E_SET_SSID: status=%u reason=%u\n",
	    msg->status, msg->reason);

	if (msg->status == CYW_E_STATUS_SUCCESS)
		return;	/* wait for E_LINK to drive to RUN */

	/* Join failed — drop the VAP back to SCAN. */
	vap = TAILQ_FIRST(&ic->ic_vaps);
	if (vap == NULL)
		return;
	if (vap->iv_state == IEEE80211_S_AUTH ||
	    vap->iv_state == IEEE80211_S_ASSOC)
		ieee80211_new_state(vap, IEEE80211_S_SCAN, 0);
}

int
cyw_security_event_attach(struct cyw_softc *sc)
{
	int err;

	err = cyw_event_register(sc, CYW_E_LINK, cyw_link_event);
	if (err != 0)
		return (err);
	err = cyw_event_register(sc, CYW_E_SET_SSID, cyw_set_ssid_event);
	if (err != 0) {
		cyw_event_unregister(sc, CYW_E_LINK);
		return (err);
	}
	return (0);
}

void
cyw_security_event_detach(struct cyw_softc *sc)
{
	cyw_event_unregister(sc, CYW_E_SET_SSID);
	cyw_event_unregister(sc, CYW_E_LINK);
}
