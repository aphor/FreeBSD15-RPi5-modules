/*
 * cyw43455_fwil.c — Firmware IOVAR/IOCTL interface layer
 *
 * Sits between callers (cfg, scan, security) and the SDPCM TX/RX primitives.
 * Encodes BCDC frames for both IOVAR (WLC_GET_VAR / WLC_SET_VAR) and raw
 * firmware commands.  Credit management and doorbell live below this layer.
 *
 * Each public cyw_fil_* call is synchronous: it builds a TX frame, sends it
 * via F2, rings the TOSBMAILBOX doorbell, and polls F2 for the matching BCDC
 * response before returning.  The callout in cyw43455_sdpcm.c does not run
 * during attach (started after cyw_fw_download), so there is no RX race.
 *
 * Reference: /usr/src/sys/contrib/dev/broadcom/brcm80211/brcmfmac/fwil.c
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"

#include "cyw43455_var.h"

#define ALIGN4(x)	(((x) + 3) & ~3)

/* -------------------------------------------------------------------------
 * cyw_sdpcm_recv_one — read one SDPCM frame from F2.
 *
 * Gates on INTSTATUS I_HMB_SW_MASK | I_XMTDATA_AVAIL before issuing CMD53.
 * On a reload the chip sits at INTSTATUS=0xa0400000 (I_CHIPACTIVE only)
 * until the new firmware completes WL init and writes HMB_DATA_FWREADY.
 * cyw_fw_download() polls for HMB_DATA_FWREADY before returning, so by
 * the time cyw_sdpcm_attach() starts the callout INTSTATUS always has the
 * data-available bits set (observed 0x00c000c0 at FWREADY).  The gate is
 * therefore safe and necessary: without it the callout issues CMD53 reads
 * to an empty F2 FIFO every 50 ms, corrupting the SDIO CAM queue state
 * and producing a "camq_remove: out-of-bounds index" panic.
 *
 * Called only during synchronous IOVAR/IOCTL transactions (before the
 * background callout starts, or while it is idle).
 *
 * Reference: brcmf_sdio_readframes() / brcmf_sdio_read_control() in sdio.c
 * ------------------------------------------------------------------------- */
int
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
		/*
		 * Gate on data-available or mailbox bits.  Without this gate the
		 * background callout issues CMD53 reads to an empty F2 FIFO every
		 * 50 ms, which corrupts the SDIO CAM queue and panics the kernel
		 * ("camq_remove: out-of-bounds index").  The gate is safe to use
		 * because cyw_sdpcm_attach() is not called until after the FWREADY
		 * handshake, at which point INTSTATUS always has I_HMB_SW_MASK and
		 * I_XMTDATA_AVAIL set (observed: 0x00c000c0 at FWREADY time).
		 */
		if ((intstatus & (I_HMB_SW_MASK | I_XMTDATA_AVAIL)) == 0)
			return (EAGAIN);
		if (intstatus & I_HMB_SW_MASK)
			cyw_bp_write32(sc, sc->sdio_core_base + SD_REG_INTSTATUS,
			    intstatus & I_HMB_SW_MASK);
	}

	err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
	    CYW_F2_BLKSIZE, buf, false /* FIFO, fixed addr */);
	if (err) {
		cyw_rx_eio_diag(sc, CYW_F2_BLKSIZE, err, "head");
		return (err);
	}

	hdr = (struct cyw_sdpcm_hdr *)buf;
	flen = le16toh(hdr->len);

	if (flen == 0 && le16toh(hdr->len_inv) == 0)
		return (EAGAIN);
	if ((uint16_t)~(flen ^ le16toh(hdr->len_inv)) != 0) {
		CYW_LOCK(sc);
		sc->rx_eagain_count++;
		CYW_UNLOCK(sc);
		return (EAGAIN);
	}
	if (flen == 0xFFFF || flen < CYW_SDPCM_HDR_LEN) {
		CYW_LOCK(sc);
		sc->rx_eagain_count++;
		CYW_UNLOCK(sc);
		return (EAGAIN);
	}
	if (flen > CYW_SDPCM_MAX_FRAME)
		return (EINVAL);

	if (flen > CYW_F2_BLKSIZE) {
		rdlen = ((size_t)(flen - CYW_F2_BLKSIZE) + CYW_F2_BLKSIZE - 1) &
		    ~(size_t)(CYW_F2_BLKSIZE - 1);
		err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
		    rdlen, buf + CYW_F2_BLKSIZE, false);
		if (err) {
			cyw_rx_eio_diag(sc, rdlen, err, "tail");
			return (err);
		}
	}

	CYW_LOCK(sc);
	sc->sdpcm_rx_max = hdr->credit;
	sc->rx_ok_count++;
	sc->rx_last_ok_ticks = ticks;
	CYW_UNLOCK(sc);

	if (out_flen != NULL)
		*out_flen = flen;
	return (0);
}

/* -------------------------------------------------------------------------
 * cyw_rx_eio_diag — on F2 EIO, snapshot chip-side state to discriminate
 * between Contingency A (KSO cleared), B (RX FIFO stall) and C (stale
 * intstat).  Issues only cheap F1 byte reads.  Rate-limited: prints at
 * most once per second to avoid flooding dmesg under sustained failure.
 *
 * See /Users/aphor/.claude/plans/floofy-whistling-scott.md and
 * doc/cyw43455.md §16 for the classification rules.
 * ------------------------------------------------------------------------- */
void
cyw_rx_eio_diag(struct cyw_softc *sc, size_t rdlen, int err, const char *tag)
{
	static int last_print_ticks;
	uint8_t  sleepcsr, framectrl, rbc_lo, rbc_hi;
	uint32_t intstatus;
	int e1 = 0, e2 = 0, e3 = 0, e4 = 0;

	CYW_LOCK(sc);
	sc->rx_eio_count++;
	sc->rx_last_eio_ticks = ticks;
	CYW_UNLOCK(sc);

	/* Rate limit: at most one diagnostic line per second */
	if (last_print_ticks != 0 &&
	    (ticks - last_print_ticks) < hz)
		return;
	last_print_ticks = ticks;

	sleepcsr  = sdio_read_1(sc->f1, SBSDIO_FUNC1_SLEEPCSR,    &e1);
	framectrl = sdio_read_1(sc->f1, SBSDIO_FUNC1_FRAMECTRL,   &e2);
	rbc_lo    = sdio_read_1(sc->f1, SBSDIO_FUNC1_RFRAMEBCLO,  &e3);
	rbc_hi    = sdio_read_1(sc->f1, SBSDIO_FUNC1_RFRAMEBCHI,  &e4);

	intstatus = (sc->sdio_core_base != 0)
	    ? cyw_bp_read32(sc, sc->sdio_core_base + SD_REG_INTSTATUS)
	    : 0xffffffffu;

	device_printf(sc->dev,
	    "RX EIO[%s] rdlen=%zu err=%d SLEEPCSR=0x%02x%s%s "
	    "FRAMECTRL=0x%02x RBC=%u INTSTAT=0x%08x (f1errs=%d/%d/%d/%d)\n",
	    tag, rdlen, err,
	    sleepcsr,
	    (sleepcsr & SBSDIO_FUNC1_SLEEPCSR_KSO_EN) ? " KSO" : " !KSO",
	    (sleepcsr & SBSDIO_FUNC1_SLEEPCSR_DEVON_MASK) ? " DEVON" : " !DEVON",
	    framectrl,
	    ((unsigned)rbc_hi << 8) | rbc_lo,
	    intstatus,
	    e1, e2, e3, e4);
}

/* -------------------------------------------------------------------------
 * cyw_fil_txrx — encode and send one IOVAR/IOCTL, poll for the response.
 *
 * cmd:        WLC_GET_VAR, WLC_SET_VAR, or a raw firmware command code.
 * bcdc_flags: 0 (GET) or BCDC_DCMD_SET (SET).
 * name:       IOVAR name string, or NULL for raw commands.
 * buf:        GET: output buffer (zero-filled in TX frame, filled from response).
 *             SET: input data (copied into TX frame after the name).
 * buflen:     byte length of buf.
 * ------------------------------------------------------------------------- */
static int
cyw_fil_txrx(struct cyw_softc *sc, uint32_t cmd, uint32_t bcdc_flags,
    const char *name, void *buf, size_t buflen)
{
	struct cyw_sdpcm_hdr *sph;
	struct cyw_bcdc_hdr  *bch;
	size_t namelen  = (name != NULL) ? strlen(name) + 1 : 0;
	size_t payload  = namelen + buflen;
	size_t framelen = ALIGN4(CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN + payload);
	uint8_t *frame, *data_start;
	uint16_t id;
	int err, i;

	frame = malloc(framelen, M_CYW43455, M_WAITOK | M_ZERO);

	sx_xlock(&sc->ioctl_sx);
	id = ++sc->ioctl_id;

	sph = (struct cyw_sdpcm_hdr *)frame;
	sph->len         = htole16((uint16_t)framelen);
	sph->len_inv     = htole16(~(uint16_t)framelen);
	sph->chan_flags  = CYW_SDPCM_CHAN_CTRL;
	sph->data_offset = CYW_SDPCM_HDR_LEN;

	bch = (struct cyw_bcdc_hdr *)(frame + CYW_SDPCM_HDR_LEN);
	bch->cmd   = htole32(cmd);
	bch->len   = htole32((uint32_t)payload);
	bch->flags = htole32(bcdc_flags | ((uint32_t)id << BCDC_DCMD_ID_SHIFT));

	data_start = frame + CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN;
	if (name != NULL)
		memcpy(data_start, name, namelen);
	/* SET: copy caller data; GET: data area stays zero (firmware fills it). */
	if (buf != NULL && (bcdc_flags & BCDC_DCMD_SET))
		memcpy(data_start + namelen, buf, buflen);

	if (sc->sdpcm_running) {
		/*
		 * Runtime path — the background sdpcm_task owns the F2 FIFO.
		 * We must not call cyw_sdpcm_recv_one here; instead we set up
		 * delivery state and sleep on ioctl_cv until the task signals.
		 */

		/* Wait for a TX credit; task keeps rx_max current. */
		for (i = 0; i < 200; i++) {
			if ((uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq) > 0)
				break;
			DELAY(5000);
		}
		if ((uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq) == 0) {
			device_printf(sc->dev,
			    "cyw_fil: no TX credits (runtime) cmd %u\n", cmd);
			err = ENOBUFS;
			goto out;
		}

		CYW_LOCK(sc);
		KASSERT(!sc->ioctl_waiting, ("cyw_fil: concurrent IOCTL"));
		sph->seq             = sc->sdpcm_tx_seq++;
		sc->ioctl_waiting    = true;
		sc->ioctl_wait_id    = id;
		sc->ioctl_resp_buf   = buf;
		sc->ioctl_resp_buflen = buflen;
		sc->ioctl_get        = !(bcdc_flags & BCDC_DCMD_SET) && (buf != NULL);
		sc->ioctl_result     = ETIMEDOUT;
		CYW_UNLOCK(sc);

		/*
		 * Acquire f2_sx before writing the IOCTL frame so that
		 * cyw_sdpcm_task (rx_tq) cannot simultaneously read F2.
		 * Release BEFORE sleeping on ioctl_cv — sdpcm_task needs
		 * to acquire f2_sx to drain the firmware response.
		 */
		sx_xlock(&sc->f2_sx);
		err = cyw_f2_write_block(sc, frame, framelen);
		if (err != 0) {
			sx_xunlock(&sc->f2_sx);
			CYW_LOCK(sc);
			sc->ioctl_waiting = false;
			CYW_UNLOCK(sc);
			goto out;
		}

		if (sc->sdio_core_base != 0)
			cyw_bp_write32(sc,
			    sc->sdio_core_base + SD_REG_TOSBMAILBOX, SMB_HOST_INT);
		sx_xunlock(&sc->f2_sx);

		CYW_LOCK(sc);
		if (sc->ioctl_waiting) {
			int cerr = cv_timedwait(&sc->ioctl_cv, &sc->mtx, 3 * hz);
			if (cerr != 0)
				sc->ioctl_waiting = false;
		}
		err = sc->ioctl_result;
		CYW_UNLOCK(sc);

		if (err == ETIMEDOUT)
			device_printf(sc->dev, "cyw_fil: timeout cmd %u '%s'\n",
			    cmd, name != NULL ? name : "");
		else if (err == EIO)
			device_printf(sc->dev,
			    "cyw_fil: firmware rejected cmd %u '%s' "
			    "status 0x%08x\n",
			    cmd, name != NULL ? name : "",
			    sc->ioctl_fw_status);

	} else {
		/*
		 * Boot-time path — called before cyw_sdpcm_attach starts the
		 * callout.  We own F2 exclusively and poll it directly.
		 */
		uint8_t *rsp;

		/* Extra block: second CMD53 appends at buf+CYW_F2_BLKSIZE. */
		rsp = malloc(CYW_SDPCM_BUF_SIZE, M_CYW43455, M_WAITOK | M_ZERO);

		sph->seq = sc->sdpcm_tx_seq;

		/* Drain RX until firmware grants at least one TX credit. */
		for (i = 0; i < 100; i++) {
			if ((uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq) > 0)
				break;
			err = cyw_sdpcm_recv_one(sc, rsp, NULL);
			if (err != 0 && err != EAGAIN)
				goto out_poll;
			DELAY(10000);
		}
		if ((uint8_t)(sc->sdpcm_rx_max - sc->sdpcm_tx_seq) == 0) {
			device_printf(sc->dev,
			    "cyw_fil: no TX credits for cmd %u\n", cmd);
			err = ENOBUFS;
			goto out_poll;
		}

		sc->sdpcm_tx_seq++;

		err = cyw_f2_write_block(sc, frame, framelen);
		if (err)
			goto out_poll;

		if (sc->sdio_core_base != 0)
			cyw_bp_write32(sc,
			    sc->sdio_core_base + SD_REG_TOSBMAILBOX, SMB_HOST_INT);

		/* Poll F2 for the BCDC response matching transaction id. */
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
				device_printf(sc->dev,
				    "cyw_fil: firmware rejected cmd %u '%s' "
				    "status 0x%08x\n",
				    cmd, name != NULL ? name : "",
				    le32toh(rbch->status));
				err = EIO;
				break;
			}

			if (buf != NULL && (bcdc_flags & BCDC_DCMD_SET) == 0) {
				uint8_t *pp = (uint8_t *)rbch + CYW_BCDC_HDR_LEN;
				size_t avail = (size_t)(rsp + flen - pp);
				memcpy(buf, pp, MIN(buflen, avail));
			}
			err = 0;
			break;
		}

		if (err == ETIMEDOUT)
			device_printf(sc->dev, "cyw_fil: timeout cmd %u '%s'\n",
			    cmd, name != NULL ? name : "");
out_poll:
		free(rsp, M_CYW43455);
	}
out:
	sx_xunlock(&sc->ioctl_sx);
	free(frame, M_CYW43455);
	return (err);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int
cyw_fil_iovar_data_get(struct cyw_softc *sc, const char *name,
    void *buf, size_t len)
{
	return (cyw_fil_txrx(sc, WLC_GET_VAR, 0, name, buf, len));
}

int
cyw_fil_iovar_data_set(struct cyw_softc *sc, const char *name,
    const void *buf, size_t len)
{
	return (cyw_fil_txrx(sc, WLC_SET_VAR, BCDC_DCMD_SET, name,
	    __DECONST(void *, buf), len));
}

int
cyw_fil_iovar_int_get(struct cyw_softc *sc, const char *name, uint32_t *val)
{
	uint32_t tmp = 0;
	int err;

	err = cyw_fil_iovar_data_get(sc, name, &tmp, sizeof(tmp));
	if (err == 0)
		*val = le32toh(tmp);
	return (err);
}

int
cyw_fil_iovar_int_set(struct cyw_softc *sc, const char *name, uint32_t val)
{
	uint32_t tmp = htole32(val);

	return (cyw_fil_iovar_data_set(sc, name, &tmp, sizeof(tmp)));
}

int
cyw_fil_cmd_data_get(struct cyw_softc *sc, uint32_t cmd,
    void *buf, size_t len)
{
	return (cyw_fil_txrx(sc, cmd, 0, NULL, buf, len));
}

int
cyw_fil_cmd_data_set(struct cyw_softc *sc, uint32_t cmd,
    const void *buf, size_t len)
{
	return (cyw_fil_txrx(sc, cmd, BCDC_DCMD_SET, NULL,
	    __DECONST(void *, buf), len));
}

int
cyw_fil_cmd_int_set(struct cyw_softc *sc, uint32_t cmd, uint32_t val)
{
	uint32_t le_val = htole32(val);
	return (cyw_fil_cmd_data_set(sc, cmd, &le_val, sizeof(le_val)));
}
