/*
 * cyw43455_fw.c — Firmware, NVRAM, and CLM blob loading for CYW43455
 *
 * Sequence:
 *   1. Load brcmfmac43455-sdio.bin from /boot/firmware/cyw43455/, write to
 *      chip RAM via F1
 *   2. Load brcmfmac43455-sdio.txt (NVRAM), parse and write to RAM end
 *   3. Release ARM CR4 — firmware boots
 *   4. Wait for F2 ready bit
 *   5. Enable F2
 *   6. Send "ver" IOVAR via BCDC, copy version string to sc->fw_version
 *   7. Load brcmfmac43455-sdio.clm_blob via chunked clmload IOVAR
 *
 * Firmware files are NOT embedded in the .ko.  They are obtained through the
 * FreeBSD firmware(9) subsystem so the regulatory blob (.clm_blob) can be
 * swapped per-deployment without rebuilding the driver.  firmware_get(9):
 *
 *   - returns loader-preloaded images directly from RAM (no VFS), so the
 *     driver works even when cyw43455 is preloaded from /boot/loader.conf
 *     before the root filesystem is mounted; and
 *   - for a post-boot kldload, lazily reads the blob from /boot/firmware/
 *     on the dedicated firmware taskqueue (which has filesystem context),
 *     never on this attach thread.
 *
 * This replaces an earlier vn_open()/vn_rdwr() reader that panicked when the
 * driver was preloaded: the SDIO discovery taskqueue has no fd_cdir/fd_rdir,
 * so namei()/cache_fplookup() faulted (far=0x4) under Giant.  See
 * doc/ for the firmware delivery details and the loader.conf preload block.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/firmware.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"

#include "cyw43455_var.h"

/*
 * firmware(9) image names.  We request them subdirectory-relative
 * ("cyw43455/<file>") so that BOTH delivery paths resolve to the same blob:
 *   - lazy load reads  fw_path("/boot/firmware/") + name  → /boot/firmware/cyw43455/<file>
 *   - loader-preloaded images registered under their absolute /boot/firmware/
 *     path are trailing-matched by the lookup() in subr_firmware.c against
 *     this name.  The trailing-match is documented in the lookup() comment
 *     in subr_firmware.c; it is not yet in firmware(9).
 */
#define CYW_FW_SUBDIR	"cyw43455/"
#define CYW_FW_NAME	"brcmfmac43455-sdio.bin"
#define CYW_NVRAM_NAME	"brcmfmac43455-sdio.txt"
#define CYW_CLM_NAME	"brcmfmac43455-sdio.clm_blob"

/* -------------------------------------------------------------------------
 * cyw_fw_get — fetch a required firmware image via firmware(9).
 *
 * Returns a held handle (release with firmware_put), or NULL with a
 * descriptive error logged.  Safe to call from the attach path: preloaded
 * images come from RAM without sleeping; a lazy disk read is deferred to the
 * firmware taskqueue while this thread sleeps (msleep under Giant is legal).
 *
 * FIRMWARE_GET_NOWARN suppresses subr_firmware's intermediate "could not load
 * firmware image, error 8" — that is just the linker-module probe (ENOEXEC)
 * failing before it falls back to reading the binary from /boot/firmware/.
 * We never ship .ko firmware, so that probe always fails; we emit our own
 * message below only on genuine absence (NULL return).
 * ------------------------------------------------------------------------- */
static const struct firmware *
cyw_fw_get(struct cyw_softc *sc, const char *relname)
{
	const struct firmware *fw;

	fw = firmware_get_flags(relname, FIRMWARE_GET_NOWARN);
	if (fw == NULL)
		device_printf(sc->dev,
		    "firmware image \"%s\" unavailable; preload it via "
		    "/boot/loader.conf (*_type=\"firmware\") for boot-time use, "
		    "or ensure /boot/firmware/%s exists for kldload\n"
		    "(other causes: securelevel policy, "
		    "debug.firmware_max_size exceeded)\n",
		    relname, relname);
	return (fw);
}

#define CYW_CLM_CHUNK	16384	/* bytes per clmload iovar call */

/* -------------------------------------------------------------------------
 * NVRAM parsing
 *
 * The NVRAM text file is a series of KEY=VALUE\n lines.  We strip comments
 * (#) and blank lines, then pack the remainder as NUL-separated tokens.
 * The final packed block is followed by a 4-byte length token at the very
 * end of chip RAM (per Broadcom convention).
 *
 * Returns: packed buffer (caller frees with free()), sets *outlen.
 * ------------------------------------------------------------------------- */
static uint8_t *
cyw_parse_nvram(const uint8_t *raw, size_t rawlen, size_t *outlen)
{
	uint8_t *buf;
	size_t buflen, i;
	bool in_comment, at_bol;

	/*
	 * Upper-bound: packed ≤ rawlen+1 (no trailing newline), +1 extra NUL
	 * (double-NUL terminator), +pad to 4-byte boundary (up to 3), +4 token.
	 * Worst case: rawlen + 1 + 1 + 3 + 4 = rawlen + 9.
	 */
	buf = malloc(roundup(rawlen + 2, 4) + 4, M_CYW43455, M_WAITOK | M_ZERO);

	buflen = 0;
	in_comment = false;
	at_bol = true;		/* at beginning of line */
	i = 0;

	while (i < rawlen) {
		char c = raw[i++];

		if (c == '\r')
			continue;

		if (c == '\n') {
			if (!in_comment && !at_bol && buflen > 0 &&
			    buf[buflen - 1] != '\0')
				buf[buflen++] = '\0';
			in_comment = false;
			at_bol = true;
			continue;
		}

		if (at_bol && c == '#') {
			in_comment = true;
			at_bol = false;
			continue;
		}
		at_bol = false;

		if (in_comment)
			continue;

		buf[buflen++] = (uint8_t)c;
	}

	/* Terminate final token if needed */
	if (buflen > 0 && buf[buflen - 1] != '\0')
		buf[buflen++] = '\0';

	/*
	 * brcmfmac uses roundup(nvram_len + 1, 4): always adds at least one
	 * extra NUL before aligning, creating a double-NUL list terminator.
	 * Match this to ensure the firmware finds the terminator correctly.
	 */
	buflen++;	/* extra NUL (double-NUL terminator) */
	while (buflen & 3)
		buf[buflen++] = '\0';

	/*
	 * Append length token: firmware validates this 4-byte little-endian value
	 * at the end of RAM.  bits[31:16] = ~nwords, bits[15:0] = nwords, where
	 * nwords = packed_nvram_bytes / 4.  High/low halves are bitwise complements;
	 * firmware checks (hi ^ lo) == 0xFFFF.
	 */
	uint32_t nwords = (uint32_t)(buflen / 4);
	uint32_t token = ((~nwords & 0xffff) << 16) | (nwords & 0xffff);
	le32enc(buf + buflen, token);
	buflen += 4;

	*outlen = buflen;
	return (buf);
}

/* -------------------------------------------------------------------------
 * cyw_fw_images_write — load .bin and NVRAM images and write to chip RAM
 *
 * Entry: ARM CR4 halted, backplane accessible via F1.
 * Exit:  firmware binary at CYW_FW_LOAD_ADDR; packed NVRAM at top of chip RAM;
 *        *out_rstvec set to the ARM reset vector (first 4 bytes of .bin).
 * ------------------------------------------------------------------------- */
static int
cyw_fw_images_write(struct cyw_softc *sc, uint32_t *out_rstvec)
{
	const struct firmware *fwp, *nvp;
	uint8_t *nvram;
	size_t fw_size, nvlen;
	uint32_t nvram_addr;
	int err;

	fwp = cyw_fw_get(sc, CYW_FW_SUBDIR CYW_FW_NAME);
	if (fwp == NULL)
		return (ENOENT);
	fw_size = fwp->datasize;

	if (fwp->datasize < 4) {
		device_printf(sc->dev,
		    "firmware image too small (%zu bytes)\n", fw_size);
		firmware_put(fwp, FIRMWARE_UNLOAD);
		return (EFTYPE);
	}
	if (fw_size > sc->ram_size) {
		device_printf(sc->dev,
		    "firmware too large (%zu > %u)\n", fw_size, sc->ram_size);
		firmware_put(fwp, FIRMWARE_UNLOAD);
		return (EFBIG);
	}

	*out_rstvec = le32dec(fwp->data);

	CYW_DPRINTF(sc, CYW_DBG_FW, "downloading firmware %s (%zu bytes)\n",
	    CYW_FW_NAME, fw_size);

	err = cyw_bp_write_block(sc, CYW_FW_LOAD_ADDR,
	    (const uint8_t *)fwp->data, fw_size);
	firmware_put(fwp, FIRMWARE_UNLOAD);
	if (err != 0) {
		device_printf(sc->dev, "firmware write failed: %d\n", err);
		return (err);
	}

	if (sc->sc_debug & CYW_DBG_FW) {
		uint32_t rb0  = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 0);
		uint32_t rb4  = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 4);
		uint32_t rb8  = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 8);
		uint32_t rb40 = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 0x40);
		device_printf(sc->dev,
		    "fw readback: [0]=%08x [4]=%08x [8]=%08x [40]=%08x"
		    " (rstvec=%08x)\n", rb0, rb4, rb8, rb40, *out_rstvec);
	}

	nvp = cyw_fw_get(sc, CYW_FW_SUBDIR CYW_NVRAM_NAME);
	if (nvp == NULL)
		return (ENOENT);
	if (nvp->datasize == 0) {
		device_printf(sc->dev, "NVRAM image is empty\n");
		firmware_put(nvp, FIRMWARE_UNLOAD);
		return (EFTYPE);
	}

	nvram = cyw_parse_nvram((const uint8_t *)nvp->data, nvp->datasize,
	    &nvlen);
	firmware_put(nvp, FIRMWARE_UNLOAD);

	/*
	 * NVRAM lives at the top of chip TCM RAM so that the firmware finds it
	 * at (ram_base + ram_size - nvlen).
	 */
	nvram_addr = sc->ram_base + sc->ram_size - (uint32_t)nvlen;
	CYW_DPRINTF(sc, CYW_DBG_FW, "writing NVRAM (%zu bytes) at 0x%08x\n",
	    nvlen, nvram_addr);

	err = cyw_bp_write_block(sc, nvram_addr, nvram, nvlen);
	free(nvram, M_CYW43455);
	if (err != 0) {
		device_printf(sc->dev, "NVRAM write failed: %d\n", err);
		return (err);
	}

	return (0);
}

/* -------------------------------------------------------------------------
 * cyw_f2_bringup — force HT, kick TOSBMAILBOXDATA, enable F2, set watermarks
 *
 * Entry: ARM CR4 released, firmware executing.
 * Exit:  F2 enabled; RX/TX watermarks configured; TOSBMAILBOXDATA kicked.
 *
 * BCM43455 is SR-capable: HT_AVAIL (0x80) never asserts under firmware; use
 * SBSDIO_FORCE_HT (0x02) directly.  Do NOT poll HT_AVAIL — expected absent.
 * Reference: brcmf_sdio_bus_init() / brcmf_sdio_htclk() (sdio.c:~4220).
 *
 * Note: the original code wrote 0x00 to CCCR register 0x06 here, intending
 * to disable interrupts (Int Enable = SDIO_CCCR_IENx = 0x04).  Register 0x06
 * is I/O Abort (SDIO spec §6.9), not Int Enable — that write was a no-op.
 * Bring-up is verified correct without it.  If spurious card interrupts appear
 * during download, the correct register is SDIO_CCCR_IENx (0x04).
 * ------------------------------------------------------------------------- */
static int
cyw_f2_bringup(struct cyw_softc *sc)
{
	uint8_t csr, devctl;
	int err, w_err;

	/* Force HT clock — SR chips skip the HT_AVAIL poll (sdio.c:~1625) */
	err = 0;
	sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_FORCE_HT, &err);
	DELAY(65);
	csr = sdio_read_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, &err);
	CYW_DPRINTF(sc, CYW_DBG_BRINGUP, "FORCE_HT: CSR=0x%02x\n", csr);
	if (err != 0) {
		device_printf(sc->dev, "FORCE_HT failed: %d\n", err);
		return (err);
	}

	/*
	 * TOSBMAILBOXDATA protocol-version kick (sdio.c:4265).
	 * Firmware validates this before asserting the F2 IORx bit.
	 * F1 backplane window access after ARM release — safe.
	 */
	if (sc->sdio_core_base != 0) {
		cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_TOSBMAILBOXDATA,
		    (uint32_t)SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT);
		CYW_DPRINTF(sc, CYW_DBG_BRINGUP,
		    "TOSBMAILBOXDATA written (sdio_core=0x%08x)\n",
		    sc->sdio_core_base);
	} else {
		device_printf(sc->dev,
		    "warning: SDIO core base unknown, skipping TOSBMAILBOXDATA\n");
	}

	/* Enable F2 — sdio_enable_func polls IORx internally */
	CYW_DPRINTF(sc, CYW_DBG_BRINGUP, "enabling F2\n");
	err = sdio_enable_func(sc->f2);
	if (err != 0) {
		device_printf(sc->dev, "F2 enable failed: %d\n", err);
		return (err);
	}
	CYW_DPRINTF(sc, CYW_DBG_BRINGUP, "F2 ready\n");

	/* Watermarks and device control (brcmfmac sets these after F2 enable) */
	w_err = 0;
	sdio_write_1(sc->f1, SBSDIO_WATERMARK, CYW_F2_WATERMARK, &w_err);
	devctl = sdio_read_1(sc->f1, SBSDIO_DEVICE_CTL, &w_err);
	sdio_write_1(sc->f1, SBSDIO_DEVICE_CTL,
	    devctl | SBSDIO_DEVCTL_F2WM_ENAB, &w_err);
	sdio_write_1(sc->f1, SBSDIO_FUNC1_MESBUSYCTRL,
	    CYW_MES_WATERMARK, &w_err);
	if (w_err != 0)
		device_printf(sc->dev, "warning: watermark init error %d\n", w_err);

	return (0);
}

/* -------------------------------------------------------------------------
 * cyw_sr_init — configure Save/Restore on SR-capable chips
 *
 * Entry: F2 enabled.
 * Exit:  WAKEUPCTRL HTWAIT set; CARDCAP CMD14 enabled; CHIPCLKCSR=FORCE_HT.
 * No-op if !sc->sr_capable.
 * Reference: brcmf_sdio_sr_init(), sdio.c:3436 (non-ULP / 43455 path).
 * ------------------------------------------------------------------------- */
static void
cyw_sr_init(struct cyw_softc *sc)
{
	uint8_t val;
	int sr_err;

	if (!sc->sr_capable)
		return;

	sr_err = 0;

	/* WAKEUPCTRL: set HTWAIT (bit 1) for non-ULP chips */
	val = sdio_read_1(sc->f1, SBSDIO_FUNC1_WAKEUPCTRL, &sr_err);
	val |= (1u << SBSDIO_FUNC1_WCTRL_HTWAIT_SHIFT);
	sdio_write_1(sc->f1, SBSDIO_FUNC1_WAKEUPCTRL, val, &sr_err);

	/* CARDCAP: enable CMD14 support + extension (F0 write) */
	sdio_f0_write_1(sc->f1, SDIO_CCCR_BRCM_CARDCAP,
	    SDIO_CCCR_BRCM_CARDCAP_CMD14_SUPPORT |
	    SDIO_CCCR_BRCM_CARDCAP_CMD14_EXT, &sr_err);

	/* CHIPCLKCSR: lock HT on permanently for SR operation */
	sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, SBSDIO_FORCE_HT, &sr_err);

	CYW_DPRINTF(sc, CYW_DBG_BRINGUP, "SR init done%s\n",
	    sr_err ? " (errors)" : "");
}

/* -------------------------------------------------------------------------
 * cyw_fw_download — write images, release ARM, bring up F2
 * ------------------------------------------------------------------------- */
int
cyw_fw_download(struct cyw_softc *sc)
{
	uint32_t rstvec, tohost, intstat;
	uint8_t csr_pre;
	int err, err_pre, hms;

	err = cyw_fw_images_write(sc, &rstvec);
	if (err != 0)
		return (err);

	/*
	 * Pre-release: ALP clock is already active from cyw_sdio_enable_clock().
	 * BCM43455 is SR-capable — HT_AVAIL (0x80) never asserts before firmware
	 * is running; don't poll for it.
	 */
	err_pre = 0;
	csr_pre = sdio_read_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, &err_pre);
	CYW_DPRINTF(sc, CYW_DBG_BRINGUP, "pre-release CSR=0x%02x\n", csr_pre);

	/*
	 * Release ARM CR4 — firmware starts executing.
	 * Do NOT touch D11; cyw_arm_halt already set it to PHYCLOCKEN state.
	 */
	cyw_arm_release(sc, rstvec);

	err = cyw_f2_bringup(sc);
	if (err != 0)
		return (err);

	cyw_sr_init(sc);

	if (sc->sdio_core_base == 0)
		return (0);

	/*
	 * HOSTINTMASK: tell firmware which INTSTATUS bits to signal on interrupt.
	 * Reference: brcmf_sdio_bus_init(), sdio.c:4276.
	 */
	cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_HOSTINTMASK,
	    I_HMB_SW_MASK | I_CHIPACTIVE);
	CYW_DPRINTF(sc, CYW_DBG_BRINGUP, "HOSTINTMASK written\n");

	/*
	 * Poll for HMB_DATA_FWREADY (bit 3 of TOHOSTMAILBOXDATA).
	 *
	 * TOHOSTMAILBOXDATA is NOT cleared by cyw_arm_halt or our INTSTATUS
	 * write in cyw_arm_release.  On reload it holds stale DEVREADY
	 * (0x00040002) from the previous firmware.  The new firmware writes
	 * FWREADY (0x00040008) ~15 ms after HOSTINTMASK is written.  Acting on
	 * stale DEVREADY causes IOVARs to time out before the firmware is ready.
	 *
	 * Reference: brcmf_sdio_hostmail() treats DEVREADY and FWREADY
	 * equivalently, but that path runs on fresh interrupt; our boot-time
	 * one-shot poll must wait for FWREADY.
	 */
	for (hms = 0; hms < 200; hms++) {
		tohost  = cyw_bp_read32(sc,
		    sc->sdio_core_base + SD_REG_TOHOSTMAILBOXDATA);
		intstat = cyw_bp_read32(sc,
		    sc->sdio_core_base + SD_REG_INTSTATUS);
		if (tohost & HMB_DATA_FWREADY) {
			CYW_DPRINTF(sc, CYW_DBG_BRINGUP,
			    "fw handshake at %d ms: TOHOST=0x%08x"
			    " INTSTATUS=0x%08x\n",
			    hms * 5, tohost, intstat);
			break;
		}
		if (hms == 199)
			device_printf(sc->dev,
			    "fw handshake timeout: TOHOST=0x%08x"
			    " INTSTATUS=0x%08x\n",
			    tohost, intstat);
		DELAY(5000);
	}

	return (0);
}

/* -------------------------------------------------------------------------
 * cyw_clm_load — upload CLM blob to firmware via chunked clmload IOVAR
 *
 * Called from cyw_attach() after cyw_fw_download() and after the boot-time
 * dongle-init IOVARs have been sent (firmware is running and responding).
 * Must be issued before cyw_sdpcm_attach() starts the RX callout.
 *
 * Wire format per Linux brcmf_c_download() / brcmf_c_download_blob():
 *   struct brcmf_dload_data_le {
 *       __le16 flag;        DL_BEGIN|DL_END | (DLOAD_HANDLER_VER<<12)
 *       __le16 dload_type;  DL_TYPE_CLM = 2
 *       __le32 len;         bytes in this chunk
 *       __le32 crc;         0 (not validated by firmware)
 *       uint8_t data[];     up to MAX_CHUNK_LEN bytes
 *   }
 *
 * Constants from Linux fwil_types.h:
 *   MAX_CHUNK_LEN       = 1400
 *   DLOAD_HANDLER_VER   = 1   (shifts to flag bits[15:12])
 *   DLOAD_FLAG_VER_SHIFT= 12
 *   DL_BEGIN            = 0x0002
 *   DL_END              = 0x0004
 *   DL_TYPE_CLM         = 2
 * ------------------------------------------------------------------------- */

/*
 * CYW_CLM_MAX_CHUNK — max CLM data bytes per clmload IOVAR call.
 *
 * sdiob uses cur_blksize=512 for F2 (independent of our CCCR write).
 * sdiob_rw_extended_sc uses block-mode CMD53 when txlen >= cur_blksize (512),
 * and byte-mode CMD53 when txlen < 512.  Block-mode CMD53 fails during the
 * boot-time polling phase with a spurious sdhci data interrupt.
 *
 * Frame overhead per chunk: SDPCM(12) + BCDC(16) + "clmload"\0(8) + dload hdr(12) = 48 B.
 * txlen = ceil(ALIGN4(48+D) / 64) * 64.
 * For D=256: ALIGN4(304)=304, ceil(304/64)*64 = 5*64 = 320 < 512.  Byte mode.  Safe.
 * For D=400: ALIGN4(448)=448, ceil(448/64)*64 = 7*64 = 448 < 512.  Byte mode.  Safe.
 * For D=401: ALIGN4(449)=452, ceil(452/64)*64 = 8*64 = 512.  Block mode.  FAILS.
 *
 * Use 256 bytes for a generous safety margin (CLM blob is 2676 bytes → 11 chunks).
 */
#define CYW_CLM_MAX_CHUNK	256
#define CYW_CLM_DLOAD_HDR_LEN	12		/* flag+type+len+crc */
#define CYW_CLM_DL_TYPE_CLM	2
#define CYW_CLM_DL_BEGIN	0x0002
#define CYW_CLM_DL_END		0x0004
#define CYW_CLM_HANDLER_VER	1
#define CYW_CLM_FLAG_VER_SHIFT	12

int
cyw_clm_load(struct cyw_softc *sc)
{
	const struct firmware *clmp;
	uint8_t *chunk_buf;
	const uint8_t *data;
	size_t datalen, cumulative;
	uint32_t chunk_len, clm_status;
	uint16_t dl_flag;
	int err;

	/*
	 * CLM blob is optional: a missing image is non-fatal (limited channels
	 * only), so request it directly and keep the message gentle rather than
	 * using cyw_fw_get()'s "preload it" warning.
	 */
	clmp = firmware_get_flags(CYW_FW_SUBDIR CYW_CLM_NAME,
	    FIRMWARE_GET_NOWARN);
	if (clmp == NULL) {
		device_printf(sc->dev,
		    "CLM blob \"%s\" not found, limited channels may be available\n",
		    CYW_CLM_NAME);
		return (0);		/* non-fatal per Linux brcmf_c_process_clm_blob */
	}

	data = clmp->data;
	datalen = clmp->datasize;

	CYW_DPRINTF(sc, CYW_DBG_FW, "loading CLM blob (%zu bytes)\n", datalen);

	/* Allocate header + max chunk */
	chunk_buf = malloc(CYW_CLM_DLOAD_HDR_LEN + CYW_CLM_MAX_CHUNK,
	    M_CYW43455, M_WAITOK | M_ZERO);

	cumulative = 0;
	dl_flag = CYW_CLM_DL_BEGIN;
	err = 0;

	do {
		if ((datalen - cumulative) > CYW_CLM_MAX_CHUNK)
			chunk_len = CYW_CLM_MAX_CHUNK;
		else {
			chunk_len = (uint32_t)(datalen - cumulative);
			dl_flag |= CYW_CLM_DL_END;
		}

		/* Build dload_data_le header */
		uint16_t flag_wire = htole16(dl_flag |
		    (uint16_t)(CYW_CLM_HANDLER_VER << CYW_CLM_FLAG_VER_SHIFT));
		uint16_t type_wire = htole16(CYW_CLM_DL_TYPE_CLM);
		uint32_t len_wire  = htole32(chunk_len);
		uint32_t crc_wire  = 0;

		memcpy(chunk_buf + 0, &flag_wire, 2);
		memcpy(chunk_buf + 2, &type_wire, 2);
		memcpy(chunk_buf + 4, &len_wire,  4);
		memcpy(chunk_buf + 8, &crc_wire,  4);
		memcpy(chunk_buf + CYW_CLM_DLOAD_HDR_LEN,
		    data + cumulative, chunk_len);

		err = cyw_fil_iovar_data_set(sc, "clmload",
		    chunk_buf, CYW_CLM_DLOAD_HDR_LEN + chunk_len);
		if (err != 0) {
			device_printf(sc->dev,
			    "CLM chunk at %zu failed: %d\n", cumulative, err);
			break;
		}

		dl_flag &= ~CYW_CLM_DL_BEGIN;
		cumulative += chunk_len;
	} while (cumulative < datalen && err == 0);

	free(chunk_buf, M_CYW43455);
	firmware_put(clmp, FIRMWARE_UNLOAD);

	if (err != 0)
		return (err);

	/* Verify: clmload_status == 0 means success */
	clm_status = 0xffffffff;
	err = cyw_fil_iovar_data_get(sc, "clmload_status",
	    &clm_status, sizeof(clm_status));
	if (err != 0) {
		device_printf(sc->dev, "clmload_status read failed: %d\n", err);
		return (err);
	}
	clm_status = le32toh(clm_status);
	if (clm_status != 0) {
		device_printf(sc->dev,
		    "CLM load failed: clmload_status=%u\n", clm_status);
		return (EIO);
	}

	CYW_DPRINTF(sc, CYW_DBG_FW, "CLM blob loaded ok\n");
	return (0);
}
