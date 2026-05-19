/*
 * cyw43455_sdpcm.c — SDPCM RX callout: channel demux and IOCTL delivery
 *
 * Callout functions run in softclock_thread where sleeping is prohibited.
 * ALL SDIO I/O (CMD52 and CMD53) sleeps via cam_periph_runccb.
 * The callout therefore only enqueues a taskqueue_thread task; the task runs
 * in a sleepable kernel thread and does all SDIO access.
 *
 * Frame dispatch:
 *   CHAN_CTRL  (0) — IOCTL response: matched by ioctl_wait_id, delivered via
 *                    ioctl_cv.  Unmatched control frames are discarded.
 *   CHAN_EVENT (1) — async firmware events: discarded (Milestone 2.4).
 *   CHAN_DATA  (2) — 802.3 Ethernet frames: discarded (Milestone 2.6).
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"

#include "cyw43455_var.h"

#define CYW_SDPCM_POLL_MS	50

/*
 * cyw_sdpcm_deliver_ctrl — called from cyw_sdpcm_task with a validated
 * CTRL-channel frame.  Matches the transaction id, copies the payload into
 * the waiting fwil buffer, and signals ioctl_cv.
 */
static void
cyw_sdpcm_deliver_ctrl(struct cyw_softc *sc, const uint8_t *buf, uint16_t flen)
{
	const struct cyw_sdpcm_hdr *hdr = (const struct cyw_sdpcm_hdr *)buf;
	const struct cyw_bcdc_hdr  *bch;
	uint16_t rsp_id;

	if (flen < CYW_SDPCM_HDR_LEN + CYW_BCDC_HDR_LEN)
		return;

	bch = (const struct cyw_bcdc_hdr *)(buf + hdr->data_offset);
	rsp_id = (uint16_t)
	    ((le32toh(bch->flags) >> BCDC_DCMD_ID_SHIFT) & 0xffff);

	CYW_LOCK(sc);
	if (!sc->ioctl_waiting || rsp_id != sc->ioctl_wait_id) {
		CYW_UNLOCK(sc);
		return;
	}

	if (le32toh(bch->flags) & BCDC_DCMD_ERROR) {
		device_printf(sc->dev,
		    "cyw_sdpcm: firmware error status 0x%x\n",
		    le32toh(bch->status));
		sc->ioctl_result = EIO;
	} else {
		sc->ioctl_result = 0;
		if (sc->ioctl_get && sc->ioctl_resp_buf != NULL) {
			const uint8_t *pp = (const uint8_t *)bch + CYW_BCDC_HDR_LEN;
			size_t avail = (size_t)((const uint8_t *)buf + flen - pp);
			memcpy(sc->ioctl_resp_buf, pp,
			    MIN(sc->ioctl_resp_buflen, avail));
		}
	}
	sc->ioctl_waiting = false;
	cv_signal(&sc->ioctl_cv);
	CYW_UNLOCK(sc);
}

/*
 * cyw_sdpcm_task — runs in taskqueue_thread (sleepable), does actual SDIO I/O.
 *
 * Drains all available F2 frames in one pass, dispatching each by channel.
 * Credits are updated in cyw_sdpcm_recv_one after every successful read.
 */
static void
cyw_sdpcm_task(void *arg, int pending __unused)
{
	struct cyw_softc *sc = arg;
	uint8_t *buf;
	int err;

	buf = malloc(CYW_SDPCM_BUF_SIZE, M_CYW43455, M_WAITOK | M_ZERO);

	if (sc->ioctl_waiting)
		device_printf(sc->dev, "cyw_sdpcm_task: running while ioctl_waiting\n");

	for (;;) {
		uint16_t flen;
		const struct cyw_sdpcm_hdr *hdr;
		int chan;

		err = cyw_sdpcm_recv_one(sc, buf, &flen);
		if (err == EAGAIN || err != 0)
			break;

		hdr  = (const struct cyw_sdpcm_hdr *)buf;
		chan = hdr->chan_flags & 0x0f;

		switch (chan) {
		case CYW_SDPCM_CHAN_CTRL:
			cyw_sdpcm_deliver_ctrl(sc, buf, flen);
			break;

		case CYW_SDPCM_CHAN_EVENT:
			/* Milestone 2.4: firmware event processing */
			break;

		case CYW_SDPCM_CHAN_DATA:
			/* Milestone 2.6: net80211 data input */
			break;

		default:
			device_printf(sc->dev,
			    "cyw_sdpcm: unknown channel %d, discarding\n", chan);
			break;
		}
	}

	free(buf, M_CYW43455);
}

/*
 * cyw_sdpcm_callout — fires every 50 ms, enqueues the task.
 * Must not do any SDIO I/O; callout context prohibits sleeping.
 */
static void
cyw_sdpcm_callout(void *arg)
{
	struct cyw_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->rx_task);
	callout_schedule(&sc->rx_callout,
	    howmany(CYW_SDPCM_POLL_MS * hz, 1000));
}

int
cyw_sdpcm_attach(struct cyw_softc *sc)
{
	/*
	 * Seed 4 initial TX credits (brcmfmac convention: bus->tx_max = 4).
	 * The firmware updates this via hdr->credit in its first response.
	 * Without seeding, sdpcm_rx_max==sdpcm_tx_seq==0 blocks the first TX.
	 */
	sc->sdpcm_rx_max = 4;

	cv_init(&sc->ioctl_cv, "cyw_ioctl");

	TASK_INIT(&sc->rx_task, 0, cyw_sdpcm_task, sc);
	callout_init(&sc->rx_callout, CALLOUT_MPSAFE);
	callout_reset(&sc->rx_callout,
	    howmany(CYW_SDPCM_POLL_MS * hz, 1000),
	    cyw_sdpcm_callout, sc);

	/* From this point fwil uses the condvar path, not FIFO polling. */
	sc->sdpcm_running = true;
	return (0);
}

void
cyw_sdpcm_detach(struct cyw_softc *sc)
{
	sc->sdpcm_running = false;
	callout_drain(&sc->rx_callout);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);

	/* Wake any fwil caller that might be sleeping (detach races). */
	CYW_LOCK(sc);
	if (sc->ioctl_waiting) {
		sc->ioctl_waiting = false;
		sc->ioctl_result  = ENXIO;
		cv_signal(&sc->ioctl_cv);
	}
	CYW_UNLOCK(sc);

	cv_destroy(&sc->ioctl_cv);
}
