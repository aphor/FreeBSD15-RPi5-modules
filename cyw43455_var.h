/*
 * cyw43455_var.h — CYW43455 SDIO WiFi driver: softc, constants, protocol structs
 *
 * The CYW43455 is a Cypress/Broadcom FullMAC 802.11ac + BT 5.0 combo chip
 * communicating via SDIO (WLAN) and UART HCI (Bluetooth).  This driver
 * covers the WLAN SDIO path only; Bluetooth is a separate milestone.
 *
 * SDIO function layout:
 *   F0 — Standard CIA (vendor 0x02d0, not directly driven here)
 *   F1 — Backplane (SoC register/RAM access, block size 64)
 *   F2 — WLAN packet DMA (block size 512 target, 64 during bring-up)
 *   F3 — (reserved/BT signaling on some revisions)
 */

#ifndef _CYW43455_VAR_H_
#define _CYW43455_VAR_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/condvar.h>
#include <sys/firmware.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net80211/ieee80211_var.h>

/* -------------------------------------------------------------------------
 * SDIO identification
 * ------------------------------------------------------------------------- */
#define CYW_VENDOR_BROADCOM		0x02d0
#define CYW_CHIP_ID_43455		0x4345	/* read from EROM ChipCommon */

/* -------------------------------------------------------------------------
 * Chip address map (BCM43455 / CYW43455)
 *
 * These are fixed for this chip family; an EROM scan would derive them
 * dynamically, which we defer to a later milestone.
 * ------------------------------------------------------------------------- */
#define CYW_SI_ENUM_BASE		0x18000000	/* ChipCommon / EROM base */
#define CYW_CHIPCOMMON_ID_OFF		0x00		/* chip ID register offset */

/* ARM CR4 core and its Aligner wrapper — used to halt/release the CPU */
#define CYW_ARM_CORE_BASE		0x18002000
#define CYW_ARM_WRAP_BASE		0x18102000

/* D11 (802.11 MAC) core */
#define CYW_D11_CORE_BASE		0x18001000
#define CYW_D11_WRAP_BASE		0x18101000

/* SDIO core (owns the F2 DMA engine) */
#define CYW_SDIO_CORE_BASE		0x18004000	/* EROM-verified: was 0x18003000 */
#define CYW_SDIO_WRAP_BASE		0x18104000

/* Firmware is written to SOCSRAM at the backplane RAM base address */
#define CYW_FW_LOAD_ADDR		CYW_RAM_BASE
#define CYW_RAM_BASE			0x00198000	/* physical SRAM start */
#define CYW_RAM_SIZE			0x000dc000	/* 880 KB — BCM43455 rev 6 SOCSRAM (Linux/OpenBSD: 0xdc000) */

/* -------------------------------------------------------------------------
 * BCMA wrapper register offsets (relative to wrapper base)
 * ------------------------------------------------------------------------- */
#define BCMA_IOCTL			0x0408
#define BCMA_RESET_CTL			0x0800
#define BCMA_IOCTL_CLK			0x0001
#define BCMA_IOCTL_FGC			0x0002	/* Force Gate Clock — clear after reset */
#define BCMA_IOCTL_CPUHALT		0x0020
#define BCMA_RESET_CTL_RESET		0x0001

/* D11 (802.11 MAC) core IOCTL bits — these are D11-specific, not generic BCMA */
#define BCMA_D11_IOCTL_PHYRESET		0x0004
#define BCMA_D11_IOCTL_PHYCLOCKEN	0x0008

/* -------------------------------------------------------------------------
 * F1 backplane window registers (F1 register addresses)
 * ------------------------------------------------------------------------- */
#define SBSDIO_FUNC1_SBADDRLOW		0x1000a	/* window addr bits [15:8] */
#define SBSDIO_FUNC1_SBADDRMID		0x1000b	/* window addr bits [23:16] */
#define SBSDIO_FUNC1_SBADDRHIGH		0x1000c	/* window addr bits [31:24] */
#define SBSDIO_FUNC1_SLEEPCSR		0x1001f	/* Sleep/KSO control (was wrongly 0x1000d) */
#define SBSDIO_FUNC1_CHIPCLKCSR	0x1000e	/* ALP/HT clock request/status */
#define SBSDIO_WATERMARK		0x10008	/* F2 RX watermark */
#define SBSDIO_DEVICE_CTL		0x10009	/* device control */
#define SBSDIO_FUNC1_MESBUSYCTRL	0x1001d	/* busy control */
#define SBSDIO_FUNC1_WAKEUPCTRL	0x1001e	/* SR wakeup control */
#define  SBSDIO_FUNC1_WCTRL_ALPWAIT_SHIFT	0	/* ULP chips */
#define  SBSDIO_FUNC1_WCTRL_HTWAIT_SHIFT	1	/* non-ULP (43455) */

/* SLEEPCSR (0x1001f) bits */
#define SBSDIO_FUNC1_SLEEPCSR_KSO_MASK		0x01	/* Keep SDIO On */
#define SBSDIO_FUNC1_SLEEPCSR_KSO_EN		0x01
#define SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK	0x02	/* Device-On status */

/* Backplane window: 32 KB, bit 15 selects 4-byte vs 1-byte access */
#define SBSDIO_SB_OFT_ADDR_MASK	0x00007fff
#define SBSDIO_SB_ACCESS_2_4B_FLAG	0x00008000

/* Clock CSR bits */
#define SBSDIO_FORCE_HT			0x02	/* force HT on (SR-capable chips; not a request) */
#define SBSDIO_ALP_AVAIL_REQ		0x08
#define SBSDIO_HT_AVAIL_REQ		0x10
#define SBSDIO_FORCE_HW_CLKREQ_OFF	0x20
#define SBSDIO_ALP_AVAIL		0x40
#define SBSDIO_HT_AVAIL		0x80

/* SDIO CCCR registers (F0 address space) */
#define SDIO_CCCR_IOEx			0x02	/* I/O enable */
#define SDIO_CCCR_IORx			0x03	/* I/O ready */
#define SDIO_CCCR_IENx			0x04	/* interrupt enable */
#define SDIO_CCCR_INTx			0x05	/* interrupt pending (bit N = func N) */
#define SDIO_FBR_BASE(fn)		((fn) * 0x100)
#define SDIO_FBR_BLKSIZE_LO		0x10	/* block size low byte */
#define SDIO_FBR_BLKSIZE_HI		0x11	/* block size high byte */

/* Broadcom CCCR extension (F0, sdio.h:33–36) */
#define SDIO_CCCR_BRCM_CARDCAP			0xf0
#define  SDIO_CCCR_BRCM_CARDCAP_CMD14_SUPPORT	0x02
#define  SDIO_CCCR_BRCM_CARDCAP_CMD14_EXT	0x04

/* F1 and F2 block sizes for BCM43455 */
#define CYW_F1_BLKSIZE			64
#define CYW_F2_BLKSIZE			64	/* bump to 512 once F2 is stable */

/* F2 FIFO address and transfer alignment (brcmfmac-freebsd sdpcm.c) */
#define CYW_F2_FIFO_ADDR		0x8000	/* fixed address for F2 FIFO CMD53 */

/* RX buffer size: max frame + one extra block for the two-read protocol */
#define CYW_SDPCM_BUF_SIZE		(CYW_SDPCM_MAX_FRAME + CYW_F2_BLKSIZE)

/* F2 watermark for BCM43455 (CY_435X family, sdio.c:58) */
#define CYW_F2_WATERMARK		0x40	/* was 0x60 — wrong for 435x */
#define CYW_MES_WATERMARK		0xc0	/* 0x40 watermark | 0x80 enable */
#define SBSDIO_DEVCTL_F2WM_ENAB		0x10	/* SBSDIO_DEVICE_CTL: enable F2 watermark */

/* -------------------------------------------------------------------------
 * BCMA EROM (Enumeration ROM) — read from ChipCommon EROMPTR to locate cores
 * Matches /usr/src/sys/dev/bhnd/bcma/bcma_eromreg.h
 * ------------------------------------------------------------------------- */
#define CHIPC_EROMPTR			0xfc	/* ChipCommon offset: EROM base addr */

#define BCMA_EROM_TABLE_EOF		0xf	/* end-of-table sentinel */

/* bits [2:0] of every entry: bit 0 = valid, bits [2:1] = type */
#define BCMA_EROM_ENTRY_ISVALID		0x1
#define BCMA_EROM_ENTRY_TYPE_MASK	0x6
#define BCMA_EROM_ENTRY_TYPE_CORE	0x0	/* component descriptor */
#define BCMA_EROM_ENTRY_TYPE_MPORT	0x2	/* master port descriptor */
#define BCMA_EROM_ENTRY_TYPE_REGION	0x4	/* address region descriptor */

/* Core descriptor word A (first word of a CORE entry) */
#define BCMA_EROM_COREA_ID_MASK		0x000fff00	/* bits [19:8]: core ID */
#define BCMA_EROM_COREA_ID_SHIFT	8

/* Core descriptor word B (second word) */
#define BCMA_EROM_COREB_NUM_MP_MASK	0x000001f0	/* bits [8:4]: master port count */
#define BCMA_EROM_COREB_NUM_MP_SHIFT	4
#define BCMA_EROM_COREB_NUM_DP_MASK	0x00003e00	/* bits [13:9]: device/bridge port count */
#define BCMA_EROM_COREB_NUM_DP_SHIFT	9
#define BCMA_EROM_COREB_NUM_WMP_MASK	0x0007c000	/* bits [18:14]: master wrapper count */
#define BCMA_EROM_COREB_NUM_WMP_SHIFT	14
#define BCMA_EROM_COREB_NUM_WSP_MASK	0x00f80000	/* bits [23:19]: slave wrapper count */
#define BCMA_EROM_COREB_NUM_WSP_SHIFT	19

/* Region descriptor */
#define BCMA_EROM_REGION_BASE_MASK	0xfffff000	/* bits [31:12]: region base */
#define BCMA_EROM_REGION_PORT_MASK	0x00000f00	/* bits [11:8]: port number */
#define BCMA_EROM_REGION_PORT_SHIFT	8
#define BCMA_EROM_REGION_TYPE_MASK	0x000000c0	/* bits [7:6]: type */
#define BCMA_EROM_REGION_TYPE_SHIFT	6
#define  BCMA_EROM_REGION_TYPE_DEVICE	0		/* device registers */
#define  BCMA_EROM_REGION_TYPE_SWRAP	2		/* slave wrapper (DMP agent) */
#define  BCMA_EROM_REGION_TYPE_MWRAP	3		/* master wrapper */
#define BCMA_EROM_REGION_SIZE_MASK	0x00000030	/* bits [5:4]: size encoding */
#define BCMA_EROM_REGION_SIZE_SHIFT	4
#define  BCMA_EROM_REGION_SIZE_OTHER	3		/* extended: next word is size */

#define BHND_COREID_SDIOD		0x829		/* SDIO device core */

/* -------------------------------------------------------------------------
 * ChipCommon chipcontrol select/access registers (offsets from CYW_SI_ENUM_BASE) */
#define CC_CHIPCTL_ADDR			0x660	/* select chipcontrol register N */
#define CC_CHIPCTL_DATA			0x664	/* read/write selected register */

/* SDIO core registers (relative to CYW_SDIO_CORE_BASE) */
#define SD_REG_INTSTATUS		0x020
#define SD_REG_HOSTINTMASK		0x024
#define SD_REG_TOSBMAILBOX		0x040
#define SD_REG_TOSBMAILBOXDATA		0x048
#define SD_REG_TOHOSTMAILBOXDATA	0x04c

/* SDPCM protocol version — written to tosbmailboxdata before sdio_enable_func(F2) */
#define SDPCM_PROT_VERSION		4
#define SMB_DATA_VERSION_SHIFT		16	/* kick value: 4 << 16 = 0x00040000 */

/* TOSBMAILBOX doorbell — host writes this after CMD53 TX to wake firmware */
#define SMB_HOST_INT			0x00000002	/* host interrupt / frame ready */
#define SMB_INT_ACK			0x00000002	/* ack TOHOSTMAILBOXDATA; clears it */

/* TOHOSTMAILBOXDATA — firmware writes these before asserting IEN/interrupt.
 * Bits [23:16] carry SDPCM protocol version; low bits carry device state.
 * Reference: brcmfmac/sdio.c brcmf_sdio_hostmail() */
#define HMB_DATA_DEVREADY		0x0002	/* SDPCM dispatcher started */
#define HMB_DATA_FWREADY		0x0008	/* WL init complete; firmware ready for IOVARs */

/* INTSTATUS / HOSTINTMASK bits (brcmfmac sdio.c:200-232) */
#define I_HMB_SW_MASK			0x000000f0	/* bits [7:4]: host mailbox SW ints */
#define I_HMB_FRAME_IND			(1u << 6)	/* = I_HMB_SW2: frame ready for host */
#define I_BUSPWR			(1u << 17)	/* SDIO bus power change */
#define I_XMTDATA_AVAIL			(1u << 23)	/* F2 FIFO has data (sdio.c:231) */
#define I_CHIPACTIVE			(1u << 29)	/* chip from doze to active */
#define I_SRESET			(1u << 30)	/* CCCR RES interrupt */
#define I_IOE2				(1u << 31)	/* CCCR IOE2 bit changed */

/* ARM CR4 core registers (relative to CYW_ARM_CORE_BASE) — ramsize via TCM banks */
#define ARMCR4_CAP			0x04
#define ARMCR4_BANKIDX			0x40
#define ARMCR4_BANKINFO			0x44
#define ARMCR4_BSZ_MASK			0x7f
#define ARMCR4_BSZ_MULT			8192
#define ARMCR4_BLK_1K_MASK		0x200
#define ARMCR4_TCBANB_MASK		0x00f
#define ARMCR4_TCBBNB_MASK		0x0f0

/* -------------------------------------------------------------------------
 * SDPCM frame format (12-byte header)
 *
 * Wire layout (little-endian):
 *   [0:1]  frame length (includes header)
 *   [2:3]  ~frame length (bitwise complement — sanity check)
 *   [4]    sequence number
 *   [5]    channel (bits [3:0]) and flags (bits [7:4])
 *   [6]    next-frame length hint
 *   [7]    data offset from start of SDPCM header to payload
 *   [8]    flow-control flags
 *   [9]    bus credits
 *   [10:11] reserved
 * ------------------------------------------------------------------------- */
struct cyw_sdpcm_hdr {
	uint16_t	len;
	uint16_t	len_inv;	/* ~len */
	uint8_t		seq;
	uint8_t		chan_flags;	/* chan [3:0] | flags [7:4] */
	uint8_t		next_len;
	uint8_t		data_offset;
	uint8_t		flow_ctrl;
	uint8_t		credit;
	uint16_t	reserved;
} __packed;

#define CYW_SDPCM_HDR_LEN		sizeof(struct cyw_sdpcm_hdr)	/* 12 */
#define CYW_SDPCM_MAX_FRAME		2048	/* max bytes per F2 read */
#define CYW_SDPCM_CHAN_CTRL		0	/* IOCTL / IOVAR */
#define CYW_SDPCM_CHAN_EVENT		1	/* async firmware events */
#define CYW_SDPCM_CHAN_DATA		2	/* 802.3 Ethernet frames */

/* -------------------------------------------------------------------------
 * BCDC command header (16 bytes, follows SDPCM header on control channel)
 * ------------------------------------------------------------------------- */
struct cyw_bcdc_hdr {
	uint32_t	cmd;		/* firmware command code */
	uint32_t	len;		/* payload length */
	uint32_t	flags;		/* direction | interface | id */
	uint32_t	status;		/* firmware status (response only) */
} __packed;

#define CYW_BCDC_HDR_LEN		sizeof(struct cyw_bcdc_hdr)	/* 16 */

/* BCDC flag field layout */
#define BCDC_DCMD_ERROR			0x01	/* 1 = firmware returned error */
#define BCDC_DCMD_SET			0x02	/* 1 = set, 0 = get */
#define BCDC_DCMD_IF_MASK		0xf000
#define BCDC_DCMD_IF_SHIFT		12
#define BCDC_DCMD_ID_MASK		0xffff0000
#define BCDC_DCMD_ID_SHIFT		16

/* -------------------------------------------------------------------------
 * Firmware event codes (fweh.h BRCMF_E_*)
 * Only the codes we subscribe to are listed here; add others when needed.
 * Reference: /usr/src/sys/contrib/dev/broadcom/brcm80211/brcmfmac/fweh.h
 * ------------------------------------------------------------------------- */
#define CYW_E_SET_SSID		0	/* join/leave result */
#define CYW_E_JOIN		1	/* join completed */
#define CYW_E_AUTH		3	/* 802.11 auth completed */
#define CYW_E_AUTH_IND		4	/* auth indication */
#define CYW_E_DEAUTH		5	/* deauthentication */
#define CYW_E_DEAUTH_IND	6	/* deauth indication */
#define CYW_E_ASSOC		7	/* association completed */
#define CYW_E_ASSOC_IND		8	/* assoc indication */
#define CYW_E_DISASSOC		11	/* disassociation */
#define CYW_E_DISASSOC_IND	12	/* disassoc indication */
#define CYW_E_LINK		16	/* link up/down (use flags field) */
#define CYW_E_SCAN_COMPLETE	26	/* active scan done */
#define CYW_E_PSK_SUP		46	/* 4-way handshake status */
#define CYW_E_IF		54	/* interface add/del/change */
#define CYW_E_ESCAN_RESULT	69	/* escan BSS result */

/* CYW_E_LINK flags field bits */
#define CYW_EVENT_MSG_LINK	0x01	/* 1 = link up, 0 = link down */

/* CYW_E_* status codes */
#define CYW_E_STATUS_SUCCESS		0
#define CYW_E_STATUS_FAIL		1
#define CYW_E_STATUS_TIMEOUT		2
#define CYW_E_STATUS_NO_NETWORKS	3
#define CYW_E_STATUS_ABORT		4
#define CYW_E_STATUS_PARTIAL		8	/* partial escan result */

/* Handler table size — covers event codes 0–127 */
#define CYW_EVENT_MAX_CODE	128

/* -------------------------------------------------------------------------
 * Host-endian event message (passed to registered handlers by events.c)
 * ------------------------------------------------------------------------- */
struct cyw_event_msg {
	uint32_t	code;
	uint32_t	status;
	uint32_t	reason;
	uint32_t	flags;
	uint32_t	datalen;
	uint8_t		addr[ETHER_ADDR_LEN];
	uint8_t		ifidx;
	uint8_t		bsscfgidx;
};

/*
 * Handler function signature.
 * cyw_softc is not yet defined at this point — forward-declared implicitly
 * via its use in pointer context; the full type is resolved by the time
 * any caller uses this typedef.
 */
struct cyw_softc;
typedef void (*cyw_event_handler_t)(struct cyw_softc *,
    const struct cyw_event_msg *, const void *, size_t);

/* -------------------------------------------------------------------------
 * Firmware IOCTL command codes (from brcm80211/brcmfmac/wlioctl.h)
 * ------------------------------------------------------------------------- */
#define WLC_UP				2	/* bring firmware interface up */
#define WLC_DOWN			3	/* bring firmware interface down */
#define WLC_SET_INFRA			20	/* set infrastructure mode */
#define WLC_SET_AUTH			22	/* set auth type (0=open) */
#define WLC_GET_SSID			25
#define WLC_SET_SSID			26	/* join SSID */
#define WLC_GET_CHANNEL			29
#define WLC_SET_CHANNEL			30
#define WLC_DISASSOC			52	/* deauthenticate */
#define WLC_GET_VAR			262	/* get IOVAR by name */
#define WLC_SET_VAR			263	/* set IOVAR by name */

/* -------------------------------------------------------------------------
 * Softc
 * ------------------------------------------------------------------------- */
struct cyw_softc {
	device_t		dev;		/* our F1 device (sdiob child) */
	struct sdio_func	*f1;		/* F1 backplane func struct */
	struct sdio_func	*f2;		/* F2 data func struct */
	struct mtx		mtx;		/* covers all softc fields */

	/* Backplane window state */
	uint32_t		bp_window;	/* current window base addr */

	/* Chip identity (read from ChipCommon via EROM) */
	uint16_t		chip_id;
	uint8_t			chip_rev;

	/* RAM geometry */
	uint32_t		ram_base;
	uint32_t		ram_size;

	/* Firmware */
	char			fw_version[128];

	/* sysctl */
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;

	/* RX poll callout + taskqueue task */
	struct taskqueue	*rx_tq;
	struct callout		rx_callout;
	struct task		rx_task;

	/* SDIO device core backplane base (found via EROM scan) */
	uint32_t		sdio_core_base;

	/* SDPCM state */
	uint8_t			sdpcm_tx_seq;
	uint8_t			sdpcm_rx_max;	/* credit ceiling from firmware */

	/* IOCTL transaction counter (16-bit, fits in BCDC id field) */
	uint16_t		ioctl_id;

	/* True once F1 is enabled and the SDIO channel is live */
	bool			sdio_attached;

	/* Save/Restore capable — set from PMU chipcontrol reg 3 bit 2 */
	bool			sr_capable;

	/*
	 * SDPCM runtime mode: true after cyw_sdpcm_attach starts the callout.
	 * While true, F2 is drained exclusively by cyw_sdpcm_task.  fwil uses
	 * ioctl_cv instead of polling the FIFO directly.
	 */
	bool			sdpcm_running;

	/* Condvar: sdpcm_task signals after delivering an IOCTL response */
	struct cv		ioctl_cv;
	bool			ioctl_waiting;	/* fwil is sleeping on ioctl_cv */
	uint16_t		ioctl_wait_id;	/* transaction id we're waiting for */
	uint8_t			*ioctl_resp_buf;  /* output buffer (GET) or NULL */
	size_t			ioctl_resp_buflen;
	bool			ioctl_get;	/* true = copy response payload */
	int			ioctl_result;	/* errno set by sdpcm_task */

	/* MAC address read from cur_etheraddr IOVAR during cfg attach */
	uint8_t			mac_addr[IEEE80211_ADDR_LEN];

	/* Firmware event handler table — indexed by CYW_E_* code (0–127) */
	cyw_event_handler_t	event_handlers[CYW_EVENT_MAX_CODE];

	/* net80211 state — must be last (large struct) */
	struct ieee80211com	ic;
};

#define CYW_LOCK(sc)		mtx_lock(&(sc)->mtx)
#define CYW_UNLOCK(sc)		mtx_unlock(&(sc)->mtx)
#define CYW_LOCK_ASSERT(sc)	mtx_assert(&(sc)->mtx, MA_OWNED)

/* -------------------------------------------------------------------------
 * Internal function declarations
 * ------------------------------------------------------------------------- */

/* cyw43455_sdio.c */
int  cyw_sdio_attach(struct cyw_softc *);
void cyw_sdio_detach(struct cyw_softc *);
int  cyw_sdio_enable_clock(struct cyw_softc *);
uint32_t cyw_bp_read32(struct cyw_softc *, uint32_t addr);
void     cyw_bp_write32(struct cyw_softc *, uint32_t addr, uint32_t val);
int  cyw_bp_write_block(struct cyw_softc *, uint32_t addr,
		const uint8_t *buf, size_t len);
int  cyw_f2_write_block(struct cyw_softc *, const uint8_t *buf, size_t flen);
int  cyw_sdio_f2_ready(struct cyw_softc *);
void cyw_arm_release(struct cyw_softc *, uint32_t rstvec);

/* cyw43455_fw.c */
int  cyw_fw_download(struct cyw_softc *);

/* cyw43455_sdpcm.c */
int  cyw_sdpcm_attach(struct cyw_softc *);
void cyw_sdpcm_detach(struct cyw_softc *);

/* cyw43455_fwil.c — IOVAR/IOCTL encoding layer */
int  cyw_sdpcm_recv_one(struct cyw_softc *, uint8_t *buf, uint16_t *out_flen);
int  cyw_fil_iovar_data_get(struct cyw_softc *, const char *name,
		void *buf, size_t len);
int  cyw_fil_iovar_data_set(struct cyw_softc *, const char *name,
		const void *buf, size_t len);
int  cyw_fil_iovar_int_get(struct cyw_softc *, const char *name,
		uint32_t *val);
int  cyw_fil_iovar_int_set(struct cyw_softc *, const char *name,
		uint32_t val);
int  cyw_fil_cmd_data_get(struct cyw_softc *, uint32_t cmd,
		void *buf, size_t len);
int  cyw_fil_cmd_data_set(struct cyw_softc *, uint32_t cmd,
		const void *buf, size_t len);

/* cyw43455_cfg.c — net80211 layer */
int  cyw_cfg_attach(struct cyw_softc *);
void cyw_cfg_detach(struct cyw_softc *);

/* cyw43455_events.c */
int  cyw_event_attach(struct cyw_softc *);
int  cyw_event_register(struct cyw_softc *, uint32_t code,
         cyw_event_handler_t);
void cyw_event_unregister(struct cyw_softc *, uint32_t code);
void cyw_event_dispatch(struct cyw_softc *, const uint8_t *buf,
         uint16_t flen);

MALLOC_DECLARE(M_CYW43455);

#endif /* _CYW43455_VAR_H_ */
