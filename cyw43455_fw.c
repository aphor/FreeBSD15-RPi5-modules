/*
 * cyw43455_fw.c — Firmware, NVRAM, and CLM blob loading for CYW43455
 *
 * Sequence:
 *   1. Load brcmfmac43455-sdio.bin via firmware(9), write to chip RAM via F1
 *   2. Load brcmfmac43455-sdio.txt (NVRAM), parse and write to RAM end
 *   3. Release ARM CR4 — firmware boots
 *   4. Wait for F2 ready bit
 *   5. Enable F2
 *   6. Send "ver" IOVAR via BCDC, copy version string to sc->fw_version
 *   7. Load brcmfmac43455-sdio.clm_blob via chunked clmload IOVAR
 *
 * Firmware files live in /boot/firmware/ on the Pi 5, matching the Linux
 * brcmfmac convention.  firmware(9) is told to look in /boot/firmware.
 */

#include <sys/param.h>
#include <sys/bus.h>
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

#define CYW_FW_NAME	"brcmfmac43455-sdio.bin"
#define CYW_NVRAM_NAME	"brcmfmac43455-sdio.txt"
#define CYW_CLM_NAME	"brcmfmac43455-sdio.clm_blob"

#define CYW_CLM_CHUNK	16384	/* bytes per clmload iovar call */

/* Align a size up to the nearest 4 bytes */
#define ALIGN4(x) (((x) + 3) & ~3)

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

	/* Upper-bound: packed form is no larger than the raw form */
	buf = malloc(rawlen + 4, M_CYW43455, M_WAITOK | M_ZERO);

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
	 * Append length token: firmware validates this 4-byte value at the end
	 * of RAM.  Format (little-endian): bits[31:16] = ~nwords, bits[15:0] = nwords
	 * where nwords = packed_nvram_bytes / 4.  High/low halves are complements;
	 * firmware checks that (hi ^ lo) == 0xFFFF.
	 */
	uint32_t nwords = (uint32_t)(buflen / 4);
	uint32_t token = ((~nwords & 0xffff) << 16) | (nwords & 0xffff);
	memcpy(buf + buflen, &token, 4);
	buflen += 4;

	*outlen = buflen;
	return (buf);
}

/* -------------------------------------------------------------------------
 * BCDC IOVAR helpers
 * ------------------------------------------------------------------------- */

/*
 * cyw_sdpcm_recv_one — read one SDPCM frame from the F2 FIFO.
 *
 * Gates on INTSTATUS I_HMB_SW_MASK; returns EAGAIN if no frame is indicated.
 * On a positive gate, clears the bits and reads the full frame (rounded up
 * to CYW_F2_BLKSIZE).  Updates sc->sdpcm_rx_max from hdr->credit.
 */
static int
cyw_sdpcm_recv_one(struct cyw_softc *sc, uint8_t *buf, uint16_t *out_flen)
{
	device_t parent = device_get_parent(sc->dev);
	struct cyw_sdpcm_hdr *hdr;
	uint16_t flen;
	size_t rdlen;
	int err;

	if (sc->sdio_core_base != 0) {
		uint32_t intstatus = cyw_bp_read32(sc,
		    sc->sdio_core_base + SD_REG_INTSTATUS);
		if ((intstatus & (I_HMB_SW_MASK | I_XMTDATA_AVAIL)) == 0)
			return (EAGAIN);
		/* Clear only the soft-mailbox bits; I_XMTDATA_AVAIL auto-clears. */
		if (intstatus & I_HMB_SW_MASK)
			cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_INTSTATUS,
			    intstatus & I_HMB_SW_MASK);
	}

	/*
	 * Peek: read enough to get the SDPCM frame length (first 2 bytes),
	 * using 64 bytes so the SDHCI block size matches F2's block size.
	 */
	err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
	    CYW_F2_BLKSIZE, buf, false /* FIFO, fixed addr */);
	if (err)
		return (err);

	hdr = (struct cyw_sdpcm_hdr *)buf;
	flen = le16toh(hdr->len);

	/* Linux brcmf_sdio_hdparse: all-zero means no more frames. */
	if (flen == 0 && le16toh(hdr->len_inv) == 0)
		return (EAGAIN);

	/* HW checksum: (uint16_t)~(flen ^ ~flen) must be 0. */
	if ((uint16_t)~(flen ^ le16toh(hdr->len_inv)) != 0)
		return (EAGAIN);

	if (flen == 0xFFFF || flen < CYW_SDPCM_HDR_LEN)
		return (EAGAIN);
	if (flen > CYW_SDPCM_MAX_FRAME)
		return (EINVAL);

	if (flen <= CYW_F2_BLKSIZE) {
		/* Frame fits in the block we already read. */
		goto done;
	}

	/*
	 * F2 is a streaming FIFO — the second CMD53 continues where the
	 * first left off.  Read only the remainder (flen - 64) rounded up
	 * to a block boundary, appending at buf+64 (not overwriting buf[0]).
	 * Linux: brcmf_sdio_read_control() / brcmf_sdio_readframes() with
	 * BRCMF_FIRSTREAD=64 do exactly this.
	 */
	rdlen = ((size_t)(flen - CYW_F2_BLKSIZE) + CYW_F2_BLKSIZE - 1) &
	    ~(size_t)(CYW_F2_BLKSIZE - 1);
	err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
	    rdlen, buf + CYW_F2_BLKSIZE, false /* FIFO, append after first block */);
	if (err)
		return (err);

done:
	CYW_LOCK(sc);
	sc->sdpcm_rx_max = hdr->credit;
	CYW_UNLOCK(sc);

	if (out_flen != NULL)
		*out_flen = flen;
	return (0);
}

/*
 * cyw_iovar_get — send a BCDC GET IOVAR over F2, return response payload.
 *
 * Frame layout:
 *   [SDPCM header 12B][BCDC header 16B][name NUL-terminated][zero-padded buf]
 *
 * TX credit management: firmware grants credits via hdr->credit.  We must not
 * advance sdpcm_tx_seq past sdpcm_rx_max.  On first call sdpcm_rx_max==0 so
 * we drain RX frames to collect firmware's boot credit grant before sending.
 */
int
cyw_iovar_get(struct cyw_softc *sc, const char *name, void *buf, size_t buflen)
{
	struct cyw_sdpcm_hdr *sph;
	struct cyw_bcdc_hdr  *bch;
	size_t namelen = strlen(name) + 1;
	size_t payload  = namelen + buflen;
	size_t framelen = ALIGN4(CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN + payload);
	uint8_t *frame, *rsp;
	uint16_t id;
	int err, i;

	frame = malloc(framelen, M_CYW43455, M_WAITOK | M_ZERO);
	/* Extra CYW_F2_BLKSIZE: second read appends at buf+64, needs room. */
	rsp   = malloc(CYW_SDPCM_MAX_FRAME + CYW_F2_BLKSIZE, M_CYW43455, M_WAITOK | M_ZERO);

	id = ++sc->ioctl_id;

	sph = (struct cyw_sdpcm_hdr *)frame;
	sph->len         = htole16((uint16_t)framelen);
	sph->len_inv     = htole16(~(uint16_t)framelen);
	sph->seq         = sc->sdpcm_tx_seq;
	sph->chan_flags  = CYW_SDPCM_CHAN_CTRL;
	sph->data_offset = CYW_SDPCM_HDR_LEN;

	bch = (struct cyw_bcdc_hdr *)(frame + CYW_SDPCM_HDR_LEN);
	bch->cmd   = htole32(WLC_GET_VAR);
	bch->len   = htole32((uint32_t)payload);
	bch->flags = htole32(((uint32_t)id << BCDC_DCMD_ID_SHIFT));

	memcpy(frame + CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN, name, namelen);

	/*
	 * Drain RX until firmware grants at least one TX credit.
	 * Firmware sends a boot SDPCM frame with credit > 0; without draining
	 * it we would spin forever since sdpcm_rx_max starts at 0.
	 */
	for (i = 0; i < 100; i++) {
		uint8_t delta = (uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq);
		if (delta > 0)
			break;
		err = cyw_sdpcm_recv_one(sc, rsp, NULL);
		if (err != 0 && err != EAGAIN) {
			free(frame, M_CYW43455);
			free(rsp, M_CYW43455);
			return (err);
		}
		DELAY(10000);
	}

	if ((uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq) == 0) {
		device_printf(sc->dev,
		    "cyw_iovar_get '%s': no TX credits after drain\n", name);
		free(frame, M_CYW43455);
		free(rsp, M_CYW43455);
		return (ENOBUFS);
	}

	sc->sdpcm_tx_seq++;

	err = cyw_f2_write_block(sc, frame, framelen);
	if (err) {
		free(frame, M_CYW43455);
		free(rsp, M_CYW43455);
		return (err);
	}

	/*
	 * Ring the doorbell: write SMB_HOST_INT to TOSBMAILBOX so firmware
	 * wakes from SR and checks F2.  Without this, SR-capable chips never
	 * process the frame we just wrote.
	 */
	if (sc->sdio_core_base != 0)
		cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_TOSBMAILBOX,
		    SMB_HOST_INT);

	if (sc->sdio_core_base != 0) {
		uint32_t s = cyw_bp_read32(sc,
		    sc->sdio_core_base + SD_REG_INTSTATUS);
		device_printf(sc->dev,
		    "cyw_iovar_get '%s': post-TX INTSTATUS=0x%08x\n", name, s);
	}

	/*
	 * Poll F2 FIFO for the IOVAR response.  Blind 64-byte reads; EAGAIN
	 * means FIFO empty.  Match on CTRL channel and BCDC transaction id.
	 */
	err = ETIMEDOUT;
	for (i = 0; i < 300; i++) {
		struct cyw_sdpcm_hdr *rsph;
		struct cyw_bcdc_hdr  *rbch;
		uint16_t flen, rsp_id;
		int rerr;

		DELAY(10000);

		rerr = cyw_sdpcm_recv_one(sc, rsp, &flen);
		if (rerr == EAGAIN)
			continue;
		if (rerr != 0) {
			err = rerr;
			break;
		}

		rsph = (struct cyw_sdpcm_hdr *)rsp;
		if (flen < CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN)
			continue;
		if ((rsph->chan_flags & 0x0f) != CYW_SDPCM_CHAN_CTRL)
			continue;

		rbch = (struct cyw_bcdc_hdr *)(rsp + rsph->data_offset);
		rsp_id = (uint16_t)
		    ((le32toh(rbch->flags) >> BCDC_DCMD_ID_SHIFT) & 0xffff);
		if (rsp_id != id)
			continue;
		if (le32toh(rbch->flags) & BCDC_DCMD_ERROR) {
			err = EIO;
			break;
		}

		uint8_t *payload_ptr = (uint8_t *)rbch + CYW_BCDC_HDR_LEN;
		size_t avail = (size_t)(rsp + flen - payload_ptr);
		memcpy(buf, payload_ptr, MIN(buflen, avail));
		err = 0;
		break;
	}

	if (err == ETIMEDOUT) {
		uint32_t s = (sc->sdio_core_base != 0) ?
		    cyw_bp_read32(sc, sc->sdio_core_base + SD_REG_INTSTATUS) : 0;
		device_printf(sc->dev,
		    "cyw_iovar_get '%s': timed out INTSTATUS=0x%08x\n", name, s);
	}

	free(frame, M_CYW43455);
	free(rsp, M_CYW43455);
	return (err);
}

/* -------------------------------------------------------------------------
 * cyw_fw_download — main firmware download entry point
 * ------------------------------------------------------------------------- */
int
cyw_fw_download(struct cyw_softc *sc)
{
	const struct firmware *fw, *nvfw;
	uint8_t *nvram;
	size_t nvlen;
	uint32_t nvram_addr, rstvec;
	int err;

	/* --- Load firmware binary --- */
	fw = firmware_get(CYW_FW_NAME);
	if (fw == NULL) {
		device_printf(sc->dev, "cannot load firmware \"%s\"\n",
		    CYW_FW_NAME);
		return (ENOENT);
	}

	if (fw->datasize > sc->ram_size) {
		device_printf(sc->dev,
		    "firmware too large (%zu > %u)\n",
		    fw->datasize, sc->ram_size);
		firmware_put(fw, FIRMWARE_UNLOAD);
		return (EFBIG);
	}

	/* Save reset vector (first word of firmware) before unloading */
	rstvec = le32toh(*(const uint32_t *)fw->data);

	device_printf(sc->dev, "downloading firmware %s (%zu bytes)\n",
	    CYW_FW_NAME, fw->datasize);

	err = cyw_bp_write_block(sc, CYW_FW_LOAD_ADDR,
	    (const uint8_t *)fw->data, fw->datasize);
	firmware_put(fw, FIRMWARE_UNLOAD);
	if (err) {
		device_printf(sc->dev, "firmware write failed: %d\n", err);
		return (err);
	}

	/* Verify download: read back several words from SOCSRAM */
	{
		uint32_t rb0 = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 0);
		uint32_t rb4 = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 4);
		uint32_t rb8 = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 8);
		uint32_t rb40 = cyw_bp_read32(sc, CYW_FW_LOAD_ADDR + 0x40);
		device_printf(sc->dev,
		    "fw readback: [0]=%08x [4]=%08x [8]=%08x [40]=%08x "
		    "(rstvec=%08x)\n", rb0, rb4, rb8, rb40, rstvec);
	}

	/* --- Load and write NVRAM --- */
	nvfw = firmware_get(CYW_NVRAM_NAME);
	if (nvfw == NULL) {
		device_printf(sc->dev, "cannot load NVRAM \"%s\"\n",
		    CYW_NVRAM_NAME);
		return (ENOENT);
	}

	nvram = cyw_parse_nvram((const uint8_t *)nvfw->data, nvfw->datasize,
	    &nvlen);
	firmware_put(nvfw, FIRMWARE_UNLOAD);

	/*
	 * NVRAM is placed at the top of chip RAM so that the firmware can
	 * find it at (ram_base + ram_size - nvlen).  We write it to the
	 * chip at that backplane address.
	 */
	nvram_addr = sc->ram_base + sc->ram_size - (uint32_t)nvlen;

	device_printf(sc->dev, "writing NVRAM (%zu bytes) at 0x%08x\n",
	    nvlen, nvram_addr);

	err = cyw_bp_write_block(sc, nvram_addr, nvram, nvlen);
	free(nvram, M_CYW43455);
	if (err) {
		device_printf(sc->dev, "NVRAM write failed: %d\n", err);
		return (err);
	}

	/* --- Release ARM CR4 — firmware boots ---
	 * Do NOT touch D11 here; cyw_arm_halt already set it to PHYCLOCKEN state.
	 * brcmf_chip_cr4_set_active() does not modify D11 either.
	 */

	/*
	 * Pre-release: ALP clock is already active from cyw_sdio_enable_clock().
	 * BCM43455 is SR-capable — HT_AVAIL (0x80) never asserts before firmware
	 * is running on SR chips; don't poll for it.  Log current CSR for debug.
	 */
	{
		int err_pre = 0;
		uint8_t csr_pre;

		csr_pre = sdio_read_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, &err_pre);
		device_printf(sc->dev, "pre-release CSR=0x%02x\n", csr_pre);
	}

	cyw_arm_release(sc, rstvec);

	/*
	 * Post-release F2 bring-up — brcmf_sdio_bus_init() / brcmf_sdio_htclk()
	 * path for SR-capable chips (sdio.c:~4220).
	 *
	 * BCM43455 is SR-capable: HT_AVAIL (0x80) never asserts under firmware;
	 * use SBSDIO_FORCE_HT (0x02) instead.  Do NOT poll for HT_AVAIL — it is
	 * not a bug that it never sets; it is expected on this chip family.
	 *
	 * Sequence:
	 *   1. Disable CCCR IEN — prevent spurious card interrupt reaching SDHCI.
	 *   2. saveclk = read CHIPCLKCSR; write saveclk | SBSDIO_FORCE_HT.
	 *   3. Write tosbmailboxdata = SDPCM_PROT_VERSION<<16 (F2 kick, sdio.c:4220).
	 *      The firmware validates this handshake before asserting the F2 IORx bit.
	 *      This write is an F1 backplane window access AFTER ARM release — safe.
	 *   4. sdio_enable_func(F2) — polls IORx internally.
	 *   5. Set watermarks, DEVICE_CTL, MESBUSYCTRL.
	 *   6. sr_init: WAKEUPCTRL HTWAIT, CARDCAP CMD14, CHIPCLKCSR=FORCE_HT.
	 */

	/* Step 1: disable CCCR IEN */
	{
		int ierr = 0;
		sdio_f0_write_1(sc->f1, 0x06 /* CCCR IEN */, 0x00, &ierr);
		if (ierr)
			device_printf(sc->dev,
			    "warning: CCCR IEN disable failed: %d\n", ierr);
	}

	/*
	 * Step 2: force HT clock for SR-capable BCM43455.
	 *
	 * SR chips do not assert HT_AVAIL (0x80) — the bit is managed by the
	 * firmware PMU internally.  brcmf_sdio_htclk() for SR chips writes
	 * FORCE_HT (0x02) directly and waits 65 µs (brcmfmac/sdio.c:~1625).
	 */
	{
		int err_clk = 0;
		uint8_t csr;

		sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR,
		    SBSDIO_FORCE_HT, &err_clk);
		DELAY(65);
		csr = sdio_read_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR, &err_clk);
		device_printf(sc->dev, "FORCE_HT: CSR=0x%02x\n", csr);
		if (err_clk != 0) {
			device_printf(sc->dev, "FORCE_HT failed: %d\n", err_clk);
			return (err_clk);
		}
	}

	/* Step 3: TOSBMAILBOXDATA protocol-version kick (brcmfmac sdio.c:4265).
	 * Requires the correct SDIO core base from the EROM scan.
	 */
	if (sc->sdio_core_base != 0) {
		cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_TOSBMAILBOXDATA,
		    (uint32_t)SDPCM_PROT_VERSION << SMB_DATA_VERSION_SHIFT);
		device_printf(sc->dev,
		    "TOSBMAILBOXDATA written (sdio_core=0x%08x)\n",
		    sc->sdio_core_base);
	} else {
		device_printf(sc->dev,
		    "warning: SDIO core base unknown, skipping TOSBMAILBOXDATA\n");
	}

	/* Step 4: enable F2 */
	device_printf(sc->dev, "firmware released, enabling F2\n");
	err = sdio_enable_func(sc->f2);
	if (err) {
		device_printf(sc->dev, "F2 enable failed: %d\n", err);
		return (err);
	}
	device_printf(sc->dev, "F2 ready\n");

	/* Step 5: watermarks and device control (brcmfmac sets these after F2 enable) */
	{
		int w_err = 0;
		uint8_t devctl;

		sdio_write_1(sc->f1, SBSDIO_WATERMARK, CYW_F2_WATERMARK, &w_err);
		devctl = sdio_read_1(sc->f1, SBSDIO_DEVICE_CTL, &w_err);
		sdio_write_1(sc->f1, SBSDIO_DEVICE_CTL,
		    devctl | SBSDIO_DEVCTL_F2WM_ENAB, &w_err);
		sdio_write_1(sc->f1, SBSDIO_FUNC1_MESBUSYCTRL,
		    CYW_MES_WATERMARK, &w_err);
		if (w_err)
			device_printf(sc->dev,
			    "warning: watermark init error %d\n", w_err);
	}

	/* Step 6: SR init — brcmf_sdio_sr_init(), sdio.c:3436 (non-ULP / 43455 path) */
	if (sc->sr_capable) {
		int sr_err = 0;
		uint8_t val;

		/* WAKEUPCTRL: set HTWAIT (bit 1) for non-ULP chips */
		val = sdio_read_1(sc->f1, SBSDIO_FUNC1_WAKEUPCTRL, &sr_err);
		val |= (1u << SBSDIO_FUNC1_WCTRL_HTWAIT_SHIFT);
		sdio_write_1(sc->f1, SBSDIO_FUNC1_WAKEUPCTRL, val, &sr_err);

		/* CARDCAP: enable CMD14 support + extension (F0 write) */
		sdio_f0_write_1(sc->f1, SDIO_CCCR_BRCM_CARDCAP,
		    SDIO_CCCR_BRCM_CARDCAP_CMD14_SUPPORT |
		    SDIO_CCCR_BRCM_CARDCAP_CMD14_EXT, &sr_err);

		/* CHIPCLKCSR: lock HT on permanently for SR operation */
		sdio_write_1(sc->f1, SBSDIO_FUNC1_CHIPCLKCSR,
		    SBSDIO_FORCE_HT, &sr_err);

		device_printf(sc->dev, "SR init done%s\n",
		    sr_err ? " (errors)" : "");
	}

	/* Step 7: HOSTINTMASK (brcmfmac sdio.c:4276). */
	if (sc->sdio_core_base != 0) {
		cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_HOSTINTMASK,
		    I_HMB_SW_MASK | I_CHIPACTIVE);
		device_printf(sc->dev, "HOSTINTMASK written\n");

		/*
		 * Check handshake: firmware should set TOHOSTMAILBOXDATA to
		 * (SDPCM_PROT_VERSION<<16)|HMB_DATA_DEVREADY after it boots.
		 * Poll briefly to see if it arrives.
		 */
		for (int hms = 0; hms < 200; hms++) {
			uint32_t tohost = cyw_bp_read32(sc,
			    sc->sdio_core_base + SD_REG_TOHOSTMAILBOXDATA);
			uint32_t intstat = cyw_bp_read32(sc,
			    sc->sdio_core_base + SD_REG_INTSTATUS);
			if (tohost != 0 || (intstat & I_HMB_SW_MASK) != 0) {
				device_printf(sc->dev,
				    "fw handshake at %d ms: TOHOST=0x%08x INTSTATUS=0x%08x\n",
				    hms * 5, tohost, intstat);
				break;
			}
			if (hms == 199)
				device_printf(sc->dev,
				    "fw handshake timeout: TOHOST=0x%08x INTSTATUS=0x%08x\n",
				    tohost, intstat);
			DELAY(5000);
		}
	}

	/* --- Read firmware version via "ver" IOVAR --- */
	memset(sc->fw_version, 0, sizeof(sc->fw_version));
	err = cyw_iovar_get(sc, "ver", sc->fw_version,
	    sizeof(sc->fw_version) - 1);
	if (err) {
		device_printf(sc->dev,
		    "firmware version IOVAR failed: %d\n", err);
		/* Non-fatal: chip is running, just version unknown */
		strlcpy(sc->fw_version, "unknown", sizeof(sc->fw_version));
	}

	return (0);
}
