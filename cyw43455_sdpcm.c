/*
 * cyw43455_sdpcm.c — SDPCM RX poll callout (Milestone 1 stub)
 *
 * In Milestone 1 the SDPCM layer is minimal: a 50 ms callout that drains
 * any firmware events from F2 and discards them.  This keeps the firmware
 * alive and prevents its internal mailbox from stalling.
 *
 * Milestone 2 will add BCDC IOCTL demux, event handling, and a real data
 * path (TX/RX for net80211).
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_subr.h>
#include <dev/sdio/sdiob.h>
#include "sdio_if.h"

#include "cyw43455_var.h"

#define CYW_SDPCM_POLL_MS	50
#define CYW_SDPCM_MAX_FRAME	(2048)

static void
cyw_sdpcm_poll(void *arg)
{
	struct cyw_softc *sc = arg;
	uint8_t *buf;
	uint16_t framelen;
	struct cyw_sdpcm_hdr *hdr;
	device_t parent;
	int err;

	buf = malloc(CYW_SDPCM_MAX_FRAME, M_CYW43455, M_NOWAIT);
	if (buf == NULL)
		goto reschedule;

	parent = device_get_parent(sc->dev);

	/* Read one frame from F2 (incremental address mode) */
	err = SDIO_READ_EXTENDED(parent, 2 /* F2 */, 0,
	    CYW_SDPCM_MAX_FRAME, buf, true);
	if (err) {
		free(buf, M_CYW43455);
		goto reschedule;
	}

	hdr = (struct cyw_sdpcm_hdr *)buf;
	framelen = le16toh(hdr->len);

	if (framelen < CYW_SDPCM_HDR_LEN || framelen > CYW_SDPCM_MAX_FRAME ||
	    le16toh(hdr->len_inv) != (uint16_t)~framelen) {
		/* Invalid or empty frame — firmware not ready yet or idle */
		free(buf, M_CYW43455);
		goto reschedule;
	}

	/* Update RX credit bookkeeping */
	sc->sdpcm_rx_max = hdr->credit;

	/*
	 * Milestone 1: silently discard all received frames.
	 * Milestone 2 will dispatch by channel:
	 *   CYW_SDPCM_CHAN_CTRL  → IOCTL response wakeup
	 *   CYW_SDPCM_CHAN_EVENT → firmware event handler
	 *   CYW_SDPCM_CHAN_DATA  → net80211 input
	 */
	free(buf, M_CYW43455);

reschedule:
	callout_schedule(&sc->rx_callout,
	    howmany(CYW_SDPCM_POLL_MS * hz, 1000));
}

int
cyw_sdpcm_attach(struct cyw_softc *sc)
{
	callout_init_mtx(&sc->rx_callout, &sc->mtx, 0);
	callout_reset(&sc->rx_callout,
	    howmany(CYW_SDPCM_POLL_MS * hz, 1000),
	    cyw_sdpcm_poll, sc);
	return (0);
}

void
cyw_sdpcm_detach(struct cyw_softc *sc)
{
	callout_drain(&sc->rx_callout);
}
