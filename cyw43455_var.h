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
#include <sys/firmware.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>

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
#define CYW_SDIO_CORE_BASE		0x18003000
#define CYW_SDIO_WRAP_BASE		0x18103000

/* Firmware is written to SOCSRAM at the backplane RAM base address */
#define CYW_FW_LOAD_ADDR		CYW_RAM_BASE
#define CYW_RAM_BASE			0x00198000	/* physical SRAM start */
#define CYW_RAM_SIZE			0x000d8000	/* 864 KB — BCM43455 SOCSRAM size */

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
#define SBSDIO_FUNC1_CHIPCLKCSR	0x1000e	/* ALP/HT clock request/status */
#define SBSDIO_WATERMARK		0x10008	/* F2 RX watermark */
#define SBSDIO_DEVICE_CTL		0x10009	/* device control */
#define SBSDIO_FUNC1_MESBUSYCTRL	0x1001d	/* busy control */

/* Backplane window: 32 KB, bit 15 selects 4-byte vs 1-byte access */
#define SBSDIO_SB_OFT_ADDR_MASK	0x00007fff
#define SBSDIO_SB_ACCESS_2_4B_FLAG	0x00008000

/* Clock CSR bits */
#define SBSDIO_ALP_AVAIL_REQ		0x08
#define SBSDIO_ALP_AVAIL		0x40
#define SBSDIO_HT_AVAIL_REQ		0x10
#define SBSDIO_HT_AVAIL		0x80
#define SBSDIO_FORCE_HW_CLKREQ_OFF	0x20

/* SDIO CCCR registers (F0 address space) */
#define SDIO_CCCR_IOEx			0x02	/* I/O enable */
#define SDIO_CCCR_IORx			0x03	/* I/O ready */
#define SDIO_FBR_BASE(fn)		((fn) * 0x100)
#define SDIO_FBR_BLKSIZE_LO		0x10	/* block size low byte */
#define SDIO_FBR_BLKSIZE_HI		0x11	/* block size high byte */

/* F1 and F2 block sizes for BCM43455 */
#define CYW_F1_BLKSIZE			64
#define CYW_F2_BLKSIZE			64	/* bump to 512 once F2 is stable */

/* F2 watermark for BCM43455 */
#define CYW_F2_WATERMARK		0x60
#define CYW_MES_WATERMARK		0x50

/* ChipCommon chipcontrol select/access registers (offsets from CYW_SI_ENUM_BASE) */
#define CC_CHIPCTL_ADDR			0x660	/* select chipcontrol register N */
#define CC_CHIPCTL_DATA			0x664	/* read/write selected register */

/* SDIO core registers (relative to CYW_SDIO_CORE_BASE) */
#define SD_REG_INTSTATUS		0x020
#define SD_REG_HOSTINTMASK		0x024
#define SD_REG_TOSBMAILBOX		0x040
#define SD_REG_TOSBMAILBOXDATA		0x048
#define SD_REG_TOHOSTMAILBOXDATA	0x04c

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
 * Firmware IOCTLs and IOVARs used in Milestone 1
 * ------------------------------------------------------------------------- */
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

	/* RX poll callout */
	struct callout		rx_callout;

	/* SDPCM state */
	uint8_t			sdpcm_tx_seq;
	uint8_t			sdpcm_rx_max;	/* credit ceiling from firmware */

	/* IOCTL transaction counter (16-bit, fits in BCDC id field) */
	uint16_t		ioctl_id;

	/* True once F1 is enabled and the SDIO channel is live */
	bool			sdio_attached;
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
int  cyw_f2_write_block(struct cyw_softc *, uint32_t addr,
		const uint8_t *buf, size_t len);
int  cyw_sdio_f2_ready(struct cyw_softc *);
void cyw_arm_release(struct cyw_softc *);

/* cyw43455_fw.c */
int  cyw_fw_download(struct cyw_softc *);

/* cyw43455_sdpcm.c */
int  cyw_sdpcm_attach(struct cyw_softc *);
void cyw_sdpcm_detach(struct cyw_softc *);
int  cyw_iovar_get(struct cyw_softc *, const char *name,
		void *buf, size_t buflen);

MALLOC_DECLARE(M_CYW43455);

#endif /* _CYW43455_VAR_H_ */
