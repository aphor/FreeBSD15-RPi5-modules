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
 *   CHAN_EVENT (1) — async firmware events: dispatched via cyw_event_dispatch().
 *   CHAN_DATA  (2) — 802.3 Ethernet frames: delivered via if_input to VAP ifp.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/condvar.h>
#include <sys/epoch.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>

#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>

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
		sc->ioctl_fw_status = le32toh(bch->status);
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
 * cyw_sdpcm_copy_data — extract a CHAN_DATA payload into an mbuf.
 *
 * Called from cyw_sdpcm_task while f2_sx is held.  Returns the mbuf for
 * later delivery via ieee80211_input_all after f2_sx is released, avoiding
 * a self-deadlock: ieee80211_input_all → cyw_transmit → sx_xlock(f2_sx).
 *
 * The 4-byte BDC data header format (distinct from the 16-byte BCDC command
 * header on CHAN_CTRL):
 *   [0] flags  — (proto_ver << 4) | checksum flags
 *   [1] priority
 *   [2] flags2 — interface index
 *   [3] data_offset — 4-byte units of padding before actual payload
 *
 * Reference: Linux brcmfmac bcdc.c BCDC_HEADER_LEN=4, brcmf_proto_bcdc_header.
 */
static struct mbuf *
cyw_sdpcm_copy_data(struct cyw_softc *sc, const uint8_t *buf, uint16_t flen)
{
	const struct cyw_sdpcm_hdr *sph = (const struct cyw_sdpcm_hdr *)buf;
	const uint8_t *bdc, *frame;
	uint16_t frame_len;
	struct mbuf *m;

	if (flen < sph->data_offset + 4) {
		device_printf(sc->dev, "cyw_sdpcm_copy_data: frame too short "
		    "(flen=%u data_offset=%u)\n", flen, sph->data_offset);
		return (NULL);
	}

	bdc   = buf + sph->data_offset;		/* 4-byte BDC data header */
	frame = bdc + 4 + (uint16_t)bdc[3] * 4;	/* skip BDC hdr + padding */

	if (frame > buf + flen) {
		device_printf(sc->dev,
		    "cyw_sdpcm_copy_data: BDC offset overrun\n");
		return (NULL);
	}

	frame_len = (uint16_t)((buf + flen) - frame);
	if (frame_len < sizeof(struct ether_header))
		return (NULL);

	m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return (NULL);
	m->m_pkthdr.len = m->m_len = frame_len;
	memcpy(mtod(m, void *), frame, frame_len);
	return (m);
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
	struct mbuf *rxq, *m, **rxq_tail;
	int err;

	buf = malloc(CYW_SDPCM_BUF_SIZE, M_CYW43455, M_WAITOK | M_ZERO);
	rxq = NULL;
	rxq_tail = &rxq;

	/*
	 * Hold f2_sx exclusively while draining F2 frames.  cyw_fil_txrx on
	 * scan_tq also holds f2_sx around its cyw_f2_write_block call.
	 * Serializing here prevents concurrent F2 access that corrupts sdiob's
	 * CAM queue (camq_remove out-of-bounds index panic).
	 *
	 * Data frames are copied into mbufs but NOT delivered inside this lock:
	 * ieee80211_input_all may trigger cyw_transmit which needs f2_sx,
	 * which would self-deadlock this thread.  Collected mbufs are delivered
	 * after the lock is released below.
	 */
	sx_xlock(&sc->f2_sx);
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
			cyw_event_dispatch(sc, buf, flen);
			break;

		case CYW_SDPCM_CHAN_DATA:
			m = cyw_sdpcm_copy_data(sc, buf, flen);
			if (m != NULL) {
				*rxq_tail = m;
				rxq_tail  = &m->m_nextpkt;
			}
			break;

		default:
			device_printf(sc->dev,
			    "cyw_sdpcm: unknown channel %d, discarding\n", chan);
			break;
		}
	}
	sx_xunlock(&sc->f2_sx);

	/* Deliver data frames now that f2_sx is released. */
	while (rxq != NULL) {
		struct ieee80211vap *vap;
		struct ether_header *eh;
		if_t ifp;
		uint32_t frame_len;
		bool is_eapol;
		struct epoch_tracker et;

		m = rxq;
		rxq = m->m_nextpkt;
		m->m_nextpkt = NULL;

		/*
		 * CYW43455 is FullMAC: the firmware strips 802.11 headers and
		 * delivers raw 802.3 ethernet frames on SDPCM channel 2.  These
		 * go directly to the VAP's ifnet via if_input, NOT through
		 * ieee80211_input_all which expects 802.11 MAC frames and would
		 * silently discard ethernet frames.  Reference: freebsd-brcmfmac
		 * sdpcm.c:566-582 (brcmf_sdpcm_rx_mbuf) and sdpcm.c:471-473.
		 */
		vap = TAILQ_FIRST(&sc->ic.ic_vaps);
		if (vap == NULL || vap->iv_ifp == NULL) {
			m_freem(m);
			continue;
		}
		ifp = vap->iv_ifp;

		/*
		 * Bump RX counters BEFORE handing the mbuf off — once
		 * if_input consumes it, mtod() is no longer safe.
		 * The Ethernet header is guaranteed present:
		 * cyw_sdpcm_copy_data rejects frames shorter than
		 * sizeof(struct ether_header).
		 */
		eh = mtod(m, struct ether_header *);
		frame_len = m->m_pkthdr.len;
		is_eapol = (ntohs(eh->ether_type) == ETHERTYPE_PAE);

		sc->rx_data_frames++;
		sc->rx_data_bytes += frame_len;
		if (is_eapol)
			sc->rx_eapol_frames++;

		m->m_pkthdr.rcvif = ifp;
		NET_EPOCH_ENTER(et);
		if_input(ifp, m);
		NET_EPOCH_EXIT(et);
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

	taskqueue_enqueue(sc->rx_tq, &sc->rx_task);
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
	sx_init(&sc->f2_sx, "cyw_f2");

	/*
	 * Dedicated per-device taskqueue so the SDPCM RX task runs in its
	 * own sleepable thread.  Using the global taskqueue_thread would
	 * deadlock: sdiob enqueues device discovery on taskqueue_thread, so
	 * cyw_attach (and its cv_timedwait in cyw_fil_txrx) runs there —
	 * blocking that thread prevents our RX task from ever being scheduled.
	 */
	sc->rx_tq = taskqueue_create("cyw43455_rx", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->rx_tq);
	if (sc->rx_tq == NULL)
		return (ENOMEM);
	taskqueue_start_threads(&sc->rx_tq, 1, PI_NET, "%s rx",
	    device_get_nameunit(sc->dev));

	/*
	 * Dedicated scan taskqueue — separate from rx_tq so that
	 * scan_start_task can sleep on ioctl_cv while rx_tq's thread
	 * remains free to process SDPCM frames and signal the condvar.
	 */
	sc->scan_tq = taskqueue_create("cyw43455_scan", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->scan_tq);
	if (sc->scan_tq == NULL) {
		taskqueue_free(sc->rx_tq);
		sc->rx_tq = NULL;
		return (ENOMEM);
	}
	taskqueue_start_threads(&sc->scan_tq, 1, PI_NET, "%s scan",
	    device_get_nameunit(sc->dev));

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
	taskqueue_drain(sc->rx_tq, &sc->rx_task);
	taskqueue_free(sc->rx_tq);
	sc->rx_tq = NULL;
	if (sc->scan_tq != NULL) {
		taskqueue_free(sc->scan_tq);
		sc->scan_tq = NULL;
	}

	/* Wake any fwil caller that might be sleeping (detach races). */
	CYW_LOCK(sc);
	if (sc->ioctl_waiting) {
		sc->ioctl_waiting = false;
		sc->ioctl_result  = ENXIO;
		cv_signal(&sc->ioctl_cv);
	}
	CYW_UNLOCK(sc);

	cv_destroy(&sc->ioctl_cv);
	sx_destroy(&sc->f2_sx);
}
