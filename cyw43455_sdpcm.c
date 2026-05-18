/*
 * cyw43455_sdpcm.c — SDPCM RX poll callout (Milestone 1 stub)
 *
 * Callout functions in FreeBSD run in softclock_thread context where sleeping
 * is unconditionally prohibited — including for non-sleepable-mutex reasons.
 * ALL SDIO I/O (CMD52 and CMD53) goes through cam_periph_runccb which sleeps.
 * Therefore the callout only schedules a taskqueue_thread task; the task runs
 * in a proper sleepable kernel thread and does the actual SDIO access.
 *
 * Milestone 1: drain F2 frames that have an INTx-pending signal, discard them.
 * Milestone 2 will demux by channel (control → IOCTL wakeup, event, data).
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
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
 * cyw_sdpcm_task — runs in taskqueue_thread (sleepable), does actual SDIO I/O.
 */
static void
cyw_sdpcm_task(void *arg, int pending __unused)
{
	struct cyw_softc *sc = arg;
	uint8_t *buf;
	uint16_t framelen;
	struct cyw_sdpcm_hdr *hdr;
	device_t parent;
	int err;

	/*
	 * Peek: 4-byte read to get frame length.  Safe on an empty FIFO —
	 * a 4-byte byte-mode CMD53 returns 0x00000000 without stalling the
	 * SDHCI controller, unlike a full block-mode read.
	 */
	buf = malloc(CYW_SDPCM_MAX_FRAME, M_CYW43455, M_WAITOK | M_ZERO);
	parent = device_get_parent(sc->dev);

	err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
	    4, buf, false /* FIFO, fixed addr */);
	if (err) {
		free(buf, M_CYW43455);
		return;
	}

	framelen = le16toh(*(uint16_t *)buf);
	if (framelen == 0 || framelen == 0xFFFF ||
	    framelen < CYW_SDPCM_HDR_LEN || framelen > CYW_SDPCM_MAX_FRAME) {
		free(buf, M_CYW43455);
		return;
	}

	/* Read the full frame. */
	{
		size_t rdlen = (framelen + CYW_F2_BLKSIZE - 1) &
		    ~(size_t)(CYW_F2_BLKSIZE - 1);
		err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, CYW_F2_FIFO_ADDR,
		    rdlen, buf, false /* FIFO, fixed addr */);
		if (err) {
			free(buf, M_CYW43455);
			return;
		}
	}

	hdr = (struct cyw_sdpcm_hdr *)buf;
	framelen = le16toh(hdr->len);

	if (framelen < CYW_SDPCM_HDR_LEN || framelen > CYW_SDPCM_MAX_FRAME ||
	    le16toh(hdr->len_inv) != (uint16_t)~framelen) {
		free(buf, M_CYW43455);
		return;
	}

	CYW_LOCK(sc);
	sc->sdpcm_rx_max = hdr->credit;
	CYW_UNLOCK(sc);

	/*
	 * Milestone 1: silently discard all received frames.
	 * Milestone 2 will dispatch by channel:
	 *   CYW_SDPCM_CHAN_CTRL  → IOCTL response wakeup
	 *   CYW_SDPCM_CHAN_EVENT → firmware event handler
	 *   CYW_SDPCM_CHAN_DATA  → net80211 input
	 */
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

	TASK_INIT(&sc->rx_task, 0, cyw_sdpcm_task, sc);
	callout_init(&sc->rx_callout, CALLOUT_MPSAFE);
	callout_reset(&sc->rx_callout,
	    howmany(CYW_SDPCM_POLL_MS * hz, 1000),
	    cyw_sdpcm_callout, sc);
	return (0);
}

void
cyw_sdpcm_detach(struct cyw_softc *sc)
{
	callout_drain(&sc->rx_callout);
	taskqueue_drain(taskqueue_thread, &sc->rx_task);
}
