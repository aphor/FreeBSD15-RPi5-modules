/*
 * cyw43455_scan.c — escan implementation (Milestone 2.4)
 *
 * cyw_do_escan()        — build brcmf_escan_params_le, issue "escan" IOVAR
 * cyw_abort_escan()     — send escan ABORT action
 * cyw_escan_result_handler() — E_ESCAN_RESULT handler; feeds ieee80211_add_scan
 * cyw_scan_attach/detach — register/unregister event handler
 *
 * The CYW43455 firmware uses D11N chanspec encoding.
 * scan_start_task / scan_end_task run on sc->scan_tq (separate from rx_tq).
 *
 * Reference: /Users/aphor/src/freebsd-brcmfmac.git/src/scan.c
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

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"

#include "cyw43455_var.h"

/* -------------------------------------------------------------------------
 * D11N chanspec constants — CYW43455 firmware always uses D11N encoding
 * (matches BRCMF_CHSPEC_D11N_* from freebsd-brcmfmac/src/cfg.h)
 * ------------------------------------------------------------------------- */
#define CYW_CHSPEC_CHAN_MASK		0x00ff
#define CYW_CHSPEC_D11N_SB_MASK		0x0300
#define CYW_CHSPEC_D11N_SB_SHIFT	8
#define CYW_CHSPEC_D11N_SB_LOWER	0x01	/* after shift */
#define CYW_CHSPEC_D11N_SB_UPPER	0x02	/* after shift */
#define CYW_CHSPEC_D11N_BW_MASK		0x0c00
#define CYW_CHSPEC_D11N_BW_SHIFT	10
#define CYW_CHSPEC_D11N_BW_40		0x03	/* after shift */

/* -------------------------------------------------------------------------
 * Escan wire-format constants
 * ------------------------------------------------------------------------- */
#define CYW_ESCAN_REQ_VERSION	1
#define CYW_ESCAN_ACTION_START	1
#define CYW_ESCAN_ACTION_ABORT	3
#define CYW_DOT11_BSSTYPE_ANY	2

/* -------------------------------------------------------------------------
 * Wire-format structures (firmware expects little-endian, __packed)
 * ------------------------------------------------------------------------- */
struct cyw_ssid_le {
	uint32_t	SSID_len;
	uint8_t		SSID[32];
} __packed;

struct cyw_scan_params_le {
	struct cyw_ssid_le	ssid_le;
	uint8_t			bssid[6];
	int8_t			bss_type;
	uint8_t			scan_type;
	uint32_t		nprobes;
	uint32_t		active_time;
	uint32_t		passive_time;
	uint32_t		home_time;
	uint32_t		channel_num;
	uint16_t		channel_list[1];	/* empty; channel_num = 0 */
} __packed;

struct cyw_escan_params_le {
	uint32_t			version;
	uint16_t			action;
	uint16_t			sync_id;
	struct cyw_scan_params_le	params_le;
} __packed;

/*
 * BSS info struct — NOT __packed.  Firmware uses natural alignment;
 * sizeof == 128 bytes.  ie_offset is relative to the start of this struct.
 * Reference: struct brcmf_bss_info_le in freebsd-brcmfmac/src/cfg.h
 */
struct cyw_bss_info_le {
	uint32_t	version;
	uint32_t	length;		/* total length including IEs */
	uint8_t		BSSID[6];
	uint16_t	beacon_period;
	uint16_t	capability;
	uint8_t		SSID_len;
	uint8_t		SSID[32];
	struct {
		uint32_t count;
		uint8_t  rates[16];
	} rateset;
	uint16_t	chanspec;
	uint16_t	atim_window;
	uint8_t		dtim_period;
	int16_t		RSSI;
	int8_t		phy_noise;
	uint8_t		n_cap;
	uint32_t	nbss_cap;
	uint8_t		ctl_ch;
	uint32_t	reserved32[1];
	uint8_t		flags;
	uint8_t		reserved[3];
	uint8_t		basic_mcs[16];
	uint16_t	ie_offset;
	uint32_t	ie_length;
	int16_t		SNR;
	/* variable-length IE data follows immediately */
};

/*
 * Escan result event data (NOT __packed; all fields naturally aligned).
 * bss_info_le starts at offset 12 (4-byte aligned).
 */
struct cyw_escan_result_le {
	uint32_t		buflen;
	uint32_t		version;
	uint16_t		sync_id;
	uint16_t		bss_count;
	struct cyw_bss_info_le	bss_info_le;
};

/* -------------------------------------------------------------------------
 * Default rates IEs — injected when IE chain lacks them
 * ------------------------------------------------------------------------- */
static const uint8_t cyw_rates_2g[] = {
	IEEE80211_ELEMID_RATES, 8,
	0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24
};
static const uint8_t cyw_rates_5g[] = {
	IEEE80211_ELEMID_RATES, 8,
	0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c
};

/* -------------------------------------------------------------------------
 * cyw_chanspec_to_channel — D11N chanspec → IEEE channel number
 * Reference: brcmf_chanspec_to_channel_d11n() in scan.c
 * ------------------------------------------------------------------------- */
static int
cyw_chanspec_to_channel(uint16_t chanspec)
{
	int ch = chanspec & CYW_CHSPEC_CHAN_MASK;
	int bw = (chanspec & CYW_CHSPEC_D11N_BW_MASK) >> CYW_CHSPEC_D11N_BW_SHIFT;
	int sb = (chanspec & CYW_CHSPEC_D11N_SB_MASK) >> CYW_CHSPEC_D11N_SB_SHIFT;

	if (bw == CYW_CHSPEC_D11N_BW_40) {
		if (sb == CYW_CHSPEC_D11N_SB_LOWER) return (ch - 2);
		if (sb == CYW_CHSPEC_D11N_SB_UPPER) return (ch + 2);
	}
	return (ch);
}

/* -------------------------------------------------------------------------
 * cyw_scan_clear_discard — clear ISCAN_DISCARD in net80211 scan state
 *
 * net80211 swscan sets ISCAN_DISCARD at scan start.  Without clearing it
 * before each ieee80211_add_scan call, all firmware BSS results are dropped.
 * Reference: brcmf_clear_scan_discard() in scan.c
 * ------------------------------------------------------------------------- */
static void
cyw_scan_clear_discard(struct ieee80211com *ic)
{
	struct ieee80211_scan_state *ss = ic->ic_scan;
	u_int *flagsp;

	if (ss == NULL)
		return;
	/* ISCAN_DISCARD (bit 1) is in the private flags word immediately
	 * following the public struct ieee80211_scan_state. */
	flagsp = (u_int *)((uint8_t *)ss + sizeof(struct ieee80211_scan_state));
	*flagsp &= ~0x02u;
}

/* -------------------------------------------------------------------------
 * cyw_find_scan_channel — find struct ieee80211_channel for a channel number
 * ------------------------------------------------------------------------- */
static struct ieee80211_channel *
cyw_find_scan_channel(struct ieee80211com *ic, int chan)
{
	int freq, band;
	struct ieee80211_channel *c;

	freq = ieee80211_ieee2mhz(chan,
	    chan <= 14 ? IEEE80211_CHAN_2GHZ : IEEE80211_CHAN_5GHZ);
	band = chan <= 14 ? IEEE80211_CHAN_G : IEEE80211_CHAN_A;

	c = ieee80211_find_channel(ic, freq, band | IEEE80211_CHAN_HT20);
	if (c == NULL)
		c = ieee80211_find_channel(ic, freq, band);
	if (c == NULL)
		c = &ic->ic_channels[0];
	return (c);
}

/* -------------------------------------------------------------------------
 * cyw_parse_ies — populate ieee80211_scanparams IE pointers from raw chain
 * Reference: brcmf_parse_ies() in scan.c
 * ------------------------------------------------------------------------- */
static void
cyw_parse_ies(struct ieee80211_scanparams *sp, uint8_t *ie, uint16_t ie_len)
{
	uint8_t *p   = ie;
	uint8_t *end = ie + ie_len;
	uint8_t *valid_end = p;

	while (p + 2 <= end) {
		uint8_t id  = p[0];
		uint8_t len = p[1];

		if (p + 2 + len > end)
			break;

		switch (id) {
		case IEEE80211_ELEMID_RATES:
			sp->rates = p;
			break;
		case IEEE80211_ELEMID_XRATES:
			sp->xrates = p;
			break;
		case IEEE80211_ELEMID_COUNTRY:
			sp->country = p;
			break;
		case IEEE80211_ELEMID_RSN:
			sp->rsn = p;
			break;
		case IEEE80211_ELEMID_HTCAP:
			sp->htcap = p;
			break;
		case IEEE80211_ELEMID_HTINFO:
			sp->htinfo = p;
			break;
		case IEEE80211_ELEMID_VENDOR:
			if (len >= 4 && p[2]==0x00 && p[3]==0x50 &&
			    p[4]==0xf2 && p[5]==0x01)
				sp->wpa = p;
			if (len >= 4 && p[2]==0x00 && p[3]==0x50 &&
			    p[4]==0xf2 && p[5]==0x02)
				sp->wme = p;
			break;
		}
		p += 2 + len;
		valid_end = p;
	}

	sp->ies     = ie;
	sp->ies_len = (uint16_t)(valid_end - ie);
}

/* -------------------------------------------------------------------------
 * cyw_add_bss — synthesize a beacon frame, feed it to ieee80211_add_scan
 * Reference: brcmf_add_scan_result() in scan.c
 * ------------------------------------------------------------------------- */
static void
cyw_add_bss(struct cyw_softc *sc, const struct cyw_bss_info_le *bi,
    uint32_t bi_len)
{
	struct ieee80211com	*ic = &sc->ic;
	struct ieee80211vap	*vap;
	struct ieee80211_scanparams sp;
	struct ieee80211_frame	wh;
	uint8_t			ssid_ie[2 + 32];
	uint8_t			tstamp[8] = { 0 };
	uint16_t		chanspec, ie_off, ie_len;
	int			chan;
	int16_t			rssi;
	int8_t			noise;

	chanspec = le16toh(bi->chanspec);
	chan     = cyw_chanspec_to_channel(chanspec);
	rssi     = (int16_t)le16toh((uint16_t)bi->RSSI);
	noise    = bi->phy_noise ? bi->phy_noise : -95;

	/* Locate IE chain: use ie_offset if plausible, else assume 128-byte hdr */
	ie_off = le16toh(bi->ie_offset);
	if (ie_off < (uint16_t)sizeof(*bi) || ie_off >= (uint16_t)bi_len)
		ie_off = (uint16_t)sizeof(*bi);
	ie_len = (bi_len > ie_off) ? (uint16_t)(bi_len - ie_off) : 0;

	/* Build SSID IE from fixed header fields */
	ssid_ie[0] = IEEE80211_ELEMID_SSID;
	ssid_ie[1] = (bi->SSID_len > 32) ? 32 : bi->SSID_len;
	memcpy(&ssid_ie[2], bi->SSID, ssid_ie[1]);

	memset(&sp, 0, sizeof(sp));
	sp.tstamp  = tstamp;
	sp.ssid    = ssid_ie;
	sp.rates   = __DECONST(uint8_t *,
	    chan <= 14 ? cyw_rates_2g : cyw_rates_5g);
	sp.chan    = chan;
	sp.bchan   = chan;
	sp.capinfo = le16toh(bi->capability);
	sp.bintval = le16toh(bi->beacon_period);

	if (ie_len > 0)
		cyw_parse_ies(&sp, __DECONST(uint8_t *, (const uint8_t *)bi + ie_off),
		    ie_len);

	if (sp.rates == NULL)
		return;

	memset(&wh, 0, sizeof(wh));
	wh.i_fc[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_BEACON;
	IEEE80211_ADDR_COPY(wh.i_addr2, bi->BSSID);
	IEEE80211_ADDR_COPY(wh.i_addr3, bi->BSSID);

	vap = TAILQ_FIRST(&ic->ic_vaps);
	if (vap == NULL)
		return;

	cyw_scan_clear_discard(ic);
	ieee80211_add_scan(vap, cyw_find_scan_channel(ic, chan),
	    &sp, &wh, IEEE80211_FC0_SUBTYPE_BEACON,
	    rssi - noise, noise);
}

/* -------------------------------------------------------------------------
 * cyw_escan_result_handler — E_ESCAN_RESULT event handler
 *
 * Runs in rx_tq (sleepable).  data points to cyw_escan_result_le.
 * bss_count == 0 → scan complete; call ieee80211_scan_done.
 * Reference: brcmf_escan_result() in scan.c
 * ------------------------------------------------------------------------- */
static void
cyw_escan_result_handler(struct cyw_softc *sc, const struct cyw_event_msg *msg,
    const void *data, size_t datalen)
{
	const struct cyw_escan_result_le *result;
	const struct cyw_bss_info_le     *bi;
	struct ieee80211com		 *ic = &sc->ic;
	struct ieee80211vap		 *vap;
	uint32_t  buflen;
	uint16_t  bss_count;

	/*
	 * Dispatch on event status first.  The firmware delivers two distinct
	 * shapes of E_ESCAN_RESULT:
	 *
	 *   status == PARTIAL (8) → BSS result; data is cyw_escan_result_le
	 *                            followed by one or more bss_info_le.
	 *   any other status      → terminal scan event (SUCCESS, ABORT,
	 *                            NO_NETWORKS, TIMEOUT, ...).  data is a
	 *                            short stub (~12 B on this firmware) with
	 *                            no parseable bss_info_le.
	 *
	 * Mirrors brcmf_cfg80211_escan_handler() in Linux brcmfmac:
	 * non-PARTIAL is always treated as "scan complete" and forwarded to
	 * ieee80211_scan_done regardless of which non-PARTIAL value it is.
	 */
	if (msg->status != CYW_E_STATUS_PARTIAL) {
		CYW_LOCK(sc);
		sc->scan_active = false;
		CYW_UNLOCK(sc);

		vap = TAILQ_FIRST(&ic->ic_vaps);
		if (vap != NULL)
			ieee80211_scan_done(vap);
		return;
	}

	if (datalen < sizeof(struct cyw_escan_result_le)) {
		device_printf(sc->dev,
		    "cyw_scan: PARTIAL escan result too short: %zu\n", datalen);
		return;
	}

	result    = (const struct cyw_escan_result_le *)data;
	buflen    = le32toh(result->buflen);
	bss_count = le16toh(result->bss_count);

	if (buflen > (uint32_t)datalen)
		buflen = (uint32_t)datalen;

	if (bss_count == 0)
		return;	/* PARTIAL with no entries — nothing to add */

	/* Process BSS entries (firmware may batch multiple per event) */
	bi = &result->bss_info_le;
	while (bss_count > 0) {
		uint32_t bi_len = le32toh(bi->length);

		if (bi_len < sizeof(*bi) || bi_len > buflen)
			break;

		cyw_add_bss(sc, bi, bi_len);

		bi = (const struct cyw_bss_info_le *)
		    ((const uint8_t *)bi + bi_len);
		bss_count--;
	}
}

/* -------------------------------------------------------------------------
 * cyw_do_escan — issue "escan" IOVAR to start a firmware scan
 *
 * Must be called WITHOUT the IC lock held (IOVAR sleeps via ioctl_cv).
 * ic_scan_start drops the IC lock before calling here.
 * Reference: brcmf_do_escan() in scan.c
 * ------------------------------------------------------------------------- */
int
cyw_do_escan(struct cyw_softc *sc)
{
	struct cyw_escan_params_le *params;
	int err;

	params = malloc(sizeof(*params), M_CYW43455, M_WAITOK | M_ZERO);

	params->version = htole32(CYW_ESCAN_REQ_VERSION);
	params->action  = htole16(CYW_ESCAN_ACTION_START);

	CYW_LOCK(sc);
	params->sync_id  = htole16(sc->escan_sync_id++);
	sc->scan_active  = true;
	CYW_UNLOCK(sc);

	memset(params->params_le.bssid, 0xff, 6);
	params->params_le.bss_type    = CYW_DOT11_BSSTYPE_ANY;
	params->params_le.scan_type   = 0;		/* active scan */
	params->params_le.nprobes     = htole32((uint32_t)-1);
	params->params_le.active_time = htole32((uint32_t)-1);
	params->params_le.passive_time = htole32((uint32_t)-1);
	params->params_le.home_time   = htole32((uint32_t)-1);
	params->params_le.channel_num = 0;		/* all channels */

	err = cyw_fil_iovar_data_set(sc, "escan", params, sizeof(*params));
	if (err != 0) {
		device_printf(sc->dev, "cyw_scan: escan IOVAR failed: %d\n", err);
		CYW_LOCK(sc);
		sc->scan_active = false;
		CYW_UNLOCK(sc);
	}

	free(params, M_CYW43455);
	return (err);
}

/* -------------------------------------------------------------------------
 * cyw_abort_escan — abort a running firmware scan
 * ------------------------------------------------------------------------- */
void
cyw_abort_escan(struct cyw_softc *sc)
{
	struct cyw_escan_params_le params;

	CYW_LOCK(sc);
	if (!sc->scan_active) {
		CYW_UNLOCK(sc);
		return;
	}
	CYW_UNLOCK(sc);

	memset(&params, 0, sizeof(params));
	params.version = htole32(CYW_ESCAN_REQ_VERSION);
	params.action  = htole16(CYW_ESCAN_ACTION_ABORT);

	CYW_LOCK(sc);
	params.sync_id  = htole16(sc->escan_sync_id);
	sc->scan_active = false;
	CYW_UNLOCK(sc);

	memset(params.params_le.bssid, 0xff, 6);
	(void)cyw_fil_iovar_data_set(sc, "escan", &params, sizeof(params));
}

/* -------------------------------------------------------------------------
 * Scan start / end tasks — run on rx_tq (sleepable).
 *
 * ic_scan_start and ic_scan_end may be called from net80211's internal scan
 * taskqueue thread without holding the IC lock.  We cannot call sleeping
 * IOVARs (cyw_do_escan / cyw_abort_escan) from that context.  Instead,
 * cyw_scan_start / cyw_scan_end enqueue these tasks on scan_tq (separate
 * from rx_tq) and return immediately — enqueueing is safe from any context.
 * scan_tq must NOT share a thread with rx_tq: scan tasks sleep on ioctl_cv,
 * and rx_tq's thread must remain free to drain F2 and signal that condvar.
 * ------------------------------------------------------------------------- */
static void
cyw_scan_start_task(void *arg, int pending __unused)
{
	cyw_do_escan((struct cyw_softc *)arg);
}

static void
cyw_scan_end_task(void *arg, int pending __unused)
{
	cyw_abort_escan((struct cyw_softc *)arg);
}

/* -------------------------------------------------------------------------
 * cyw_scan_attach / cyw_scan_detach
 * ------------------------------------------------------------------------- */
int
cyw_scan_attach(struct cyw_softc *sc)
{
	TASK_INIT(&sc->scan_start_task, 0, cyw_scan_start_task, sc);
	TASK_INIT(&sc->scan_end_task,   0, cyw_scan_end_task,   sc);
	return (cyw_event_register(sc, CYW_E_ESCAN_RESULT,
	    cyw_escan_result_handler));
}

void
cyw_scan_detach(struct cyw_softc *sc)
{
	cyw_event_unregister(sc, CYW_E_ESCAN_RESULT);
	/* Drain any pending scan tasks before the softc is freed */
	if (sc->scan_tq != NULL) {
		taskqueue_drain(sc->scan_tq, &sc->scan_start_task);
		taskqueue_drain(sc->scan_tq, &sc->scan_end_task);
	}
}
