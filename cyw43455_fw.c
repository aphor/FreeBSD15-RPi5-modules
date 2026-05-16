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

	/* Pad to 4-byte alignment */
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
 * BCDC IOVAR helpers (minimal: GET only, no SDPCM framing yet)
 *
 * For Milestone 1 we need to send a single IOVAR to read the firmware
 * version.  We do this before the SDPCM poll callout starts, so we drive
 * the transaction directly here.
 * ------------------------------------------------------------------------- */

/*
 * cyw_iovar_get — send a BCDC GET IOVAR over F2, return response payload.
 *
 * Frame layout on F2:
 *   [SDPCM header 12B][BCDC header 16B][name NUL-terminated][zero-padded buf]
 */
int
cyw_iovar_get(struct cyw_softc *sc, const char *name, void *buf, size_t buflen)
{
	struct cyw_sdpcm_hdr *sph;
	struct cyw_bcdc_hdr  *bch;
	size_t namelen = strlen(name) + 1;	/* include NUL */
	size_t payload  = namelen + buflen;
	size_t framelen = ALIGN4(CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN + payload);
	uint8_t *frame, *rsp;
	uint16_t id;
	int err;

	frame = malloc(framelen, M_CYW43455, M_WAITOK | M_ZERO);
	rsp   = malloc(framelen, M_CYW43455, M_WAITOK | M_ZERO);

	id = ++sc->ioctl_id;

	/* SDPCM header */
	sph = (struct cyw_sdpcm_hdr *)frame;
	sph->len      = htole16((uint16_t)framelen);
	sph->len_inv  = htole16(~(uint16_t)framelen);
	sph->seq      = sc->sdpcm_tx_seq++;
	sph->chan_flags = CYW_SDPCM_CHAN_CTRL;
	sph->data_offset = CYW_SDPCM_HDR_LEN;

	/* BCDC header */
	bch = (struct cyw_bcdc_hdr *)(frame + CYW_SDPCM_HDR_LEN);
	bch->cmd   = htole32(WLC_GET_VAR);
	bch->len   = htole32((uint32_t)payload);
	bch->flags = htole32(((uint32_t)id << BCDC_DCMD_ID_SHIFT));
	/* BCDC_DCMD_SET is 0 → GET direction */

	/* IOVAR name */
	memcpy(frame + CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN, name, namelen);

	/* Transmit on F2 at address 0 (firmware reads from its DMA ring) */
	err = cyw_f2_write_block(sc, 0, frame, framelen);
	if (err) {
		free(frame, M_CYW43455);
		free(rsp, M_CYW43455);
		return (err);
	}

	/* Poll F2 for the response (up to 500 ms) */
	for (int retries = 50; retries > 0; retries--) {
		device_t parent = device_get_parent(sc->dev);
		DELAY(10000);	/* 10 ms */
		err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, 0,
		    framelen, rsp, true);
		if (err)
			continue;

		struct cyw_sdpcm_hdr *rsph = (struct cyw_sdpcm_hdr *)rsp;
		if (le16toh(rsph->len) < CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN)
			continue;
		if ((rsph->chan_flags & 0x0f) != CYW_SDPCM_CHAN_CTRL)
			continue;

		struct cyw_bcdc_hdr *rbch =
		    (struct cyw_bcdc_hdr *)(rsp + CYW_SDPCM_HDR_LEN);
		uint16_t rsp_id = (uint16_t)
		    ((le32toh(rbch->flags) >> BCDC_DCMD_ID_SHIFT) & 0xffff);
		if (rsp_id != id)
			continue;
		if (le32toh(rbch->flags) & BCDC_DCMD_ERROR) {
			err = EIO;
			break;
		}

		/* Copy payload back to caller */
		uint8_t *rsp_payload = rsp + CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN;
		size_t copy = MIN(buflen,
		    (size_t)(le16toh(rsph->len) -
		    CYW_SDPCM_HDR_LEN - CYW_BCDC_HDR_LEN));
		memcpy(buf, rsp_payload, copy);
		err = 0;
		break;
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
	uint32_t nvram_addr;
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

	device_printf(sc->dev, "downloading firmware %s (%zu bytes)\n",
	    CYW_FW_NAME, fw->datasize);

	err = cyw_bp_write_block(sc, CYW_FW_LOAD_ADDR,
	    (const uint8_t *)fw->data, fw->datasize);
	firmware_put(fw, FIRMWARE_UNLOAD);
	if (err) {
		device_printf(sc->dev, "firmware write failed: %d\n", err);
		return (err);
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
	cyw_arm_release(sc);

	device_printf(sc->dev, "firmware released, enabling F2\n");

	/*
	 * Write CCCR IOEx bit 2 (enable F2) BEFORE polling IORx.
	 * Per SDIO spec: host enables function via IOEx, firmware ACKs via IORx.
	 * sdio_enable_func() only writes IOEx — it does not poll IORx.
	 */
	err = sdio_enable_func(sc->f2);
	if (err) {
		device_printf(sc->dev, "F2 enable (IOEx) failed: %d\n", err);
		return (err);
	}

	/* --- Wait for firmware to signal F2 ready in IORx --- */
	err = cyw_sdio_f2_ready(sc);
	if (err) {
		device_printf(sc->dev, "firmware boot timed out\n");
		return (err);
	}

	device_printf(sc->dev, "F2 ready\n");

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
