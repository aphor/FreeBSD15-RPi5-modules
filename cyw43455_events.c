/*
 * cyw43455_events.c — firmware event dispatcher (Milestone 2.4a)
 *
 * SDPCM channel 1 (CYW_SDPCM_CHAN_EVENT) frame layout:
 *
 *   ┌──────────────┬──────────┬──────────────────────────────────────────┐
 *   │ SDPCM hdr 12B│ BDC hdr 4B│ Ethernet event frame                    │
 *   └──────────────┴──────────┴──────────────────────────────────────────┘
 *                               │
 *                               ▼
 *   ┌──────────────┬─────────────────┬────────────────────┬───────────┐
 *   │ ether_hdr 14B│ brcm_ethhdr 10B │ event_msg_be 48B   │ data ...  │
 *   └──────────────┴─────────────────┴────────────────────┴───────────┘
 *
 * Multi-byte fields in brcm_ethhdr and cyw_event_msg_be are big-endian.
 * The SDPCM header's data_offset field gives the byte offset from frame
 * start to the BDC header.  The BDC header's data_offset gives additional
 * 4-byte padding before the Ethernet frame (usually 0).
 *
 * cyw_event_attach() subscribes to events via the "event_msgs" IOVAR.
 * cyw_event_dispatch() is called by cyw_sdpcm_task() on CHAN_EVENT frames.
 * cyw_event_register() lets future milestones install per-code handlers.
 *
 * Reference: /usr/src/sys/contrib/dev/broadcom/brcm80211/brcmfmac/fweh.{c,h}
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <net/ethernet.h>

#include "cyw43455_var.h"

/* -------------------------------------------------------------------------
 * Wire-format structures (all multi-byte fields are big-endian on the wire)
 * ------------------------------------------------------------------------- */

/*
 * ETH_P_LINK_CTL (0x886c) — ether_type used by all Broadcom event frames.
 * Stored in network byte order in struct ether_header.ether_type, so the
 * raw in-memory uint16 on a little-endian host is 0x6c88.  We use
 * be16toh() when comparing.
 */
#define CYW_ETH_P_LINK_CTL	0x886c

/* Broadcom OUI for event frame validation */
static const uint8_t cyw_brcm_oui[3] = { 0x00, 0x10, 0x18 };
#define CYW_BCMILCP_BCM_SUBTYPE_EVENT	1

/*
 * BDC (Bus Data Channel) header — 4 bytes, placed between the SDPCM header
 * and the Ethernet event frame on CHAN_EVENT (and CHAN_DATA) frames.
 * data_offset: additional padding after the 4-byte BDC header, in 32-bit
 * words (almost always 0 in practice).
 */
#define CYW_BDC_HDR_LEN	4
struct cyw_bdc_hdr {
	uint8_t		flags;		/* BDC version in bits [3:2] */
	uint8_t		priority;
	uint8_t		flags2;
	uint8_t		data_offset;	/* extra padding in 4-byte units */
} __packed;

/*
 * Broadcom-specific Ethernet extension header (follows struct ether_header).
 * All multi-byte fields are big-endian.
 */
struct cyw_brcm_ethhdr {
	uint16_t	subtype;
	uint16_t	length;
	uint8_t		version;
	uint8_t		oui[3];		/* cyw_brcm_oui = { 0x00, 0x10, 0x18 } */
	uint16_t	usr_subtype;	/* CYW_BCMILCP_BCM_SUBTYPE_EVENT = 1 */
} __packed;

/*
 * Firmware event message header (big-endian on the wire).
 * Follows immediately after cyw_brcm_ethhdr.
 * event-specific data (datalen bytes) follows this struct.
 */
struct cyw_event_msg_be {
	uint16_t	version;
	uint16_t	flags;
	uint32_t	event_type;
	uint32_t	status;
	uint32_t	reason;
	uint32_t	auth_type;
	uint32_t	datalen;
	uint8_t		addr[ETHER_ADDR_LEN];
	char		ifname[IFNAMSIZ];
	uint8_t		ifidx;
	uint8_t		bsscfgidx;
} __packed;

/*
 * Minimum total frame size for a valid event:
 *   SDPCM (12) + BDC (4) + ether_header (14) + brcm_ethhdr (10) + event_msg_be (48)
 */
#define CYW_EVENT_MIN_FRAME	\
    ((size_t)(CYW_SDPCM_HDR_LEN + CYW_BDC_HDR_LEN + \
	sizeof(struct ether_header) + sizeof(struct cyw_brcm_ethhdr) + \
	sizeof(struct cyw_event_msg_be)))

/* -------------------------------------------------------------------------
 * Event name table (for diagnostic log messages)
 * ------------------------------------------------------------------------- */
static const struct { uint32_t code; const char *name; } cyw_event_names[] = {
	{ CYW_E_SET_SSID,	"E_SET_SSID"	},
	{ CYW_E_JOIN,		"E_JOIN"	},
	{ CYW_E_AUTH,		"E_AUTH"	},
	{ CYW_E_AUTH_IND,	"E_AUTH_IND"	},
	{ CYW_E_DEAUTH,		"E_DEAUTH"	},
	{ CYW_E_DEAUTH_IND,	"E_DEAUTH_IND"	},
	{ CYW_E_ASSOC,		"E_ASSOC"	},
	{ CYW_E_ASSOC_IND,	"E_ASSOC_IND"	},
	{ CYW_E_DISASSOC,	"E_DISASSOC"	},
	{ CYW_E_DISASSOC_IND,	"E_DISASSOC_IND"},
	{ CYW_E_LINK,		"E_LINK"	},
	{ CYW_E_SCAN_COMPLETE,	"E_SCAN_COMPLETE"},
	{ CYW_E_PSK_SUP,	"E_PSK_SUP"	},
	{ CYW_E_IF,		"E_IF"		},
	{ CYW_E_ESCAN_RESULT,	"E_ESCAN_RESULT"},
};

static const char *
cyw_event_name(uint32_t code)
{
	for (size_t i = 0; i < nitems(cyw_event_names); i++)
		if (cyw_event_names[i].code == code)
			return (cyw_event_names[i].name);
	return ("E_UNKNOWN");
}

/* -------------------------------------------------------------------------
 * cyw_event_attach — subscribe to firmware events via "event_msgs" IOVAR
 *
 * Called from cyw_cfg_attach() after ieee80211_ifattach().
 * The event_msgs IOVAR takes a byte-array bitmask: bit N enables event N.
 * We subscribe to all events we might ever handle (steps 4 and 5 add
 * handlers for most of them; E_IF and E_ESCAN_RESULT are needed by steps
 * 4a and 5 respectively).
 *
 * Reference: brcmf_fweh_activate_events() in fweh.c
 * ------------------------------------------------------------------------- */

/* 16 bytes covers event codes 0–127 */
#define CYW_EVENT_MASK_LEN	16

int
cyw_event_attach(struct cyw_softc *sc)
{
	uint8_t mask[CYW_EVENT_MASK_LEN];
	static const uint32_t subscribe[] = {
		CYW_E_SET_SSID,
		CYW_E_JOIN,
		CYW_E_AUTH,
		CYW_E_AUTH_IND,
		CYW_E_DEAUTH,
		CYW_E_DEAUTH_IND,
		CYW_E_ASSOC,
		CYW_E_ASSOC_IND,
		CYW_E_DISASSOC,
		CYW_E_DISASSOC_IND,
		CYW_E_LINK,
		CYW_E_SCAN_COMPLETE,
		CYW_E_PSK_SUP,
		CYW_E_IF,		/* always subscribe, per brcmfmac */
		CYW_E_ESCAN_RESULT,
	};
	int err;

	memset(mask, 0, sizeof(mask));
	for (size_t i = 0; i < nitems(subscribe); i++) {
		uint32_t c = subscribe[i];
		if (c < CYW_EVENT_MASK_LEN * 8)
			mask[c / 8] |= (uint8_t)(1u << (c % 8));
	}

	err = cyw_fil_iovar_data_set(sc, "event_msgs", mask, sizeof(mask));
	if (err != 0)
		device_printf(sc->dev,
		    "cyw_event: event_msgs IOVAR failed: %d\n", err);
	return (err);
}

/* -------------------------------------------------------------------------
 * cyw_event_register / cyw_event_unregister
 *
 * Register or clear a per-code handler.  Called by future milestones:
 *   Milestone 2.3 (scan):  registers CYW_E_ESCAN_RESULT handler
 *   Milestone 2.5 (assoc): registers CYW_E_LINK, CYW_E_SET_SSID, etc.
 *
 * Safe to call from any context; handler registration is serialised by
 * sc->mtx.  Handlers themselves are called from cyw_sdpcm_task (process
 * context, sleepable) without sc->mtx held.
 * ------------------------------------------------------------------------- */
int
cyw_event_register(struct cyw_softc *sc, uint32_t code,
    cyw_event_handler_t handler)
{
	if (code >= CYW_EVENT_MAX_CODE)
		return (EINVAL);
	CYW_LOCK(sc);
	sc->event_handlers[code] = handler;
	CYW_UNLOCK(sc);
	return (0);
}

void
cyw_event_unregister(struct cyw_softc *sc, uint32_t code)
{
	if (code >= CYW_EVENT_MAX_CODE)
		return;
	CYW_LOCK(sc);
	sc->event_handlers[code] = NULL;
	CYW_UNLOCK(sc);
}

/* -------------------------------------------------------------------------
 * cyw_event_dispatch — parse and dispatch one CHAN_EVENT frame
 *
 * Called from cyw_sdpcm_task() in cyw43455_sdpcm.c (sleepable context).
 * buf  — pointer to the start of the SDPCM frame header (buf[0..flen-1])
 * flen — validated frame length in bytes (from cyw_sdpcm_recv_one)
 *
 * Frame parsing sequence (reference: brcmf_fweh_process_event() in fweh.c):
 *  1. Locate the BDC header at buf + sdpcm_hdr->data_offset.
 *  2. Compute the Ethernet event frame start:
 *       eth_start = sdpcm_hdr->data_offset + 4 + bdc_hdr->data_offset * 4
 *  3. Validate ether_type == 0x886c (ETH_P_LINK_CTL), BRCM OUI, subtype.
 *  4. Decode the big-endian event_msg_be, log it, dispatch to handler.
 * ------------------------------------------------------------------------- */
void
cyw_event_dispatch(struct cyw_softc *sc, const uint8_t *buf, uint16_t flen)
{
	const struct cyw_sdpcm_hdr	*sph;
	const struct cyw_bdc_hdr	*bdc;
	const struct ether_header	*eth;
	const struct cyw_brcm_ethhdr	*brcm;
	const struct cyw_event_msg_be	*emsg;
	const uint8_t	*data;
	cyw_event_handler_t handler;
	struct cyw_event_msg msg;
	size_t bdc_len, eth_off;
	uint32_t code, datalen;

	if ((size_t)flen < CYW_EVENT_MIN_FRAME)
		return;

	sph = (const struct cyw_sdpcm_hdr *)buf;

	/*
	 * BDC header starts at buf[data_offset].  data_offset is the byte
	 * offset from the frame start to the first payload byte (12 for our
	 * frames; set to CYW_SDPCM_HDR_LEN in cyw_fil_txrx TX path).
	 * The BDC header's own data_offset field gives additional 4-byte
	 * padding before the Ethernet frame — 0 in practice.
	 */
	bdc     = (const struct cyw_bdc_hdr *)(buf + sph->data_offset);
	bdc_len = CYW_BDC_HDR_LEN + (size_t)bdc->data_offset * 4;
	eth_off = sph->data_offset + bdc_len;

	if (eth_off + sizeof(*eth) + sizeof(*brcm) + sizeof(*emsg) > flen)
		return;

	eth  = (const struct ether_header *)(buf + eth_off);
	brcm = (const struct cyw_brcm_ethhdr *)(eth + 1);
	emsg = (const struct cyw_event_msg_be *)(brcm + 1);
	data = (const uint8_t *)(emsg + 1);

	/*
	 * Validate Broadcom event frame (mirrors brcmf_fweh_process_skb()):
	 *   ether_type = 0x886c (ETH_P_LINK_CTL)
	 *   oui        = \x00\x10\x18 (BRCM_OUI)
	 *   usr_subtype = 1 (BCMILCP_BCM_SUBTYPE_EVENT)
	 */
	if (be16toh(eth->ether_type) != CYW_ETH_P_LINK_CTL)
		return;
	if (memcmp(brcm->oui, cyw_brcm_oui, sizeof(cyw_brcm_oui)) != 0)
		return;
	if (be16toh(brcm->usr_subtype) != CYW_BCMILCP_BCM_SUBTYPE_EVENT)
		return;

	/* Decode big-endian fields */
	code    = be32toh(emsg->event_type);
	datalen = be32toh(emsg->datalen);

	/* Clamp datalen to what's actually present in the frame */
	if ((size_t)(data - buf) + datalen > flen)
		datalen = flen - (size_t)(data - buf);

	device_printf(sc->dev,
	    "cyw_event: %s (%u) status=%u reason=%u flags=0x%02x "
	    "ifidx=%u bsscfg=%u\n",
	    cyw_event_name(code), code,
	    be32toh(emsg->status), be32toh(emsg->reason),
	    be16toh(emsg->flags), emsg->ifidx, emsg->bsscfgidx);

	/*
	 * E_IF payload: brcmf_if_event { u8 ifidx, u8 action, u8 flags,
	 *   u8 bsscfgidx, u8 role }.  action: 1=ADD, 2=DEL, 3=CHANGE.
	 * Log it so we can see whether WLC_UP fires ADD or something else.
	 */
	if (code == CYW_E_IF && datalen >= 4) {
		const uint8_t *ie = data;	/* ifidx, action, flags, bsscfgidx */
		static const char * const act[] = { "?", "ADD", "DEL", "CHANGE" };
		device_printf(sc->dev,
		    "cyw_event: E_IF payload: ifidx=%u action=%s(%u) "
		    "flags=0x%02x bsscfgidx=%u\n",
		    ie[0], (ie[1] < 4 ? act[ie[1]] : "?"), ie[1],
		    ie[2], ie[3]);
	}

	if (code >= CYW_EVENT_MAX_CODE)
		return;

	/* Snapshot handler under lock; call it without lock (may sleep) */
	CYW_LOCK(sc);
	handler = sc->event_handlers[code];
	CYW_UNLOCK(sc);

	if (handler == NULL)
		return;

	msg.code      = code;
	msg.status    = be32toh(emsg->status);
	msg.reason    = be32toh(emsg->reason);
	msg.flags     = be16toh(emsg->flags);
	msg.datalen   = datalen;
	msg.ifidx     = emsg->ifidx;
	msg.bsscfgidx = emsg->bsscfgidx;
	memcpy(msg.addr, emsg->addr, ETHER_ADDR_LEN);

	handler(sc, &msg, data, datalen);
}
