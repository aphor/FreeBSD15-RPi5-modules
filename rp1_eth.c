/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2012-2014 Thomas Skibo <thomasskibo@yahoo.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Milestone 2: Cadence GEM_GXL driver for the RP1 southbridge on
 * Raspberry Pi 5, attached via pmap_mapdev (no PCIe bus driver).
 *
 * Forked from sys/dev/cadence/if_cgem.c.
 * Changes from the original:
 *  - FDT/OFW/clock probe+attach frontend removed; replaced by rp1eth_attach()
 *    called from rp1_eth_cfg.c MOD_LOAD after M1 hardware setup.
 *  - CGEM64 not defined: RP1 PCIe2 inbound window is 32-bit; 32-bit
 *    descriptors only; DMA constrained to BUS_SPACE_MAXADDR_32BIT.
 *  - No interrupts (polled): all GEM interrupts masked; RX/TX serviced from
 *    a callout at RP1ETH_POLL_HZ.  ISR body kept for Milestone 3.
 *  - No miibus: link state polled from callout via direct MDIO reads;
 *    ifmedia set to 1000baseT-FDX (negotiated in M1).
 *  - RD4/WR4 use vm_offset_t KVA directly (no struct resource shim).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_mib.h>
#include <net/if_types.h>

#ifdef INET
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#endif

#include <net/if_var.h>
#include <net/bpf.h>
#include <net/bpfdesc.h>

/* MII register constants come from rp1_eth_var.h to avoid redefinition. */

#include "rp1_eth_hw.h"
#include "rp1_eth_var.h"	/* for rp1eth_attach_args */

/* NOTE: CGEM64 is deliberately NOT defined; 32-bit descriptors only. */

#define RP1ETH_NUM_RX_DESCS	256
#define RP1ETH_NUM_TX_DESCS	256
#define RP1ETH_DEFAULT_RXBUFS	128
#define TX_MAX_DMA_SEGS		8

/*
 * Poll interval: 200 Hz → 5 ms.  Effective throughput is limited until
 * Milestone 3 adds real interrupts.
 */
#define RP1ETH_POLL_HZ		200

MALLOC_DECLARE(M_RP1ETH);	/* declared in rp1_eth_cfg.c */

/* -----------------------------------------------------------------------
 * Softc
 * ----------------------------------------------------------------------- */
struct rp1eth_softc {
	if_t			ifp;
	struct mtx		sc_mtx;
	int			if_old_flags;

	/* Hardware access (no struct resource; direct KVA access). */
	vm_offset_t		mac_kva;

	/* Identity set from FDT at MOD_LOAD. */
	uint8_t			macaddr[ETHER_ADDR_LEN];
	int			phy_addr;
	int			link_up;	/* last polled link state */

	/* Media. */
	struct ifmedia		ifmedia;

	/* Polling callout. */
	struct callout		tick_ch;
	int			poll_tick;	/* counter; wraps at RP1ETH_POLL_HZ */

	/* GEM register shadow copies. */
	uint32_t		net_ctl_shadow;
	uint32_t		net_cfg_shadow;

	/* Null queue support for priority queues we cannot disable. */
	int			neednullqs;

	/* DMA tags. */
	bus_dma_tag_t		desc_dma_tag;
	bus_dma_tag_t		mbuf_dma_tag;

	/* Receive descriptor ring. */
	struct cgem_rx_desc	*rxring;
	bus_addr_t		rxring_physaddr;
	struct mbuf		*rxring_m[RP1ETH_NUM_RX_DESCS];
	bus_dmamap_t		rxring_m_dmamap[RP1ETH_NUM_RX_DESCS];
	int			rxring_hd_ptr;
	int			rxring_tl_ptr;
	int			rxring_queued;
	bus_dmamap_t		rxring_dma_map;
	int			rxbufs;
	u_int			rxoverruns;
	u_int			rxnobufs;
	u_int			rxdmamapfails;

	/* Transmit descriptor ring. */
	struct cgem_tx_desc	*txring;
	bus_addr_t		txring_physaddr;
	struct mbuf		*txring_m[RP1ETH_NUM_TX_DESCS];
	bus_dmamap_t		txring_m_dmamap[RP1ETH_NUM_TX_DESCS];
	int			txring_hd_ptr;
	int			txring_tl_ptr;
	int			txring_queued;
	u_int			txfull;
	u_int			txdefrags;
	u_int			txdefragfails;
	u_int			txdmamapfails;

	/* Null descriptor rings (for unused priority queues). */
	void			*null_qs;
	bus_addr_t		null_qs_physaddr;

	/* Hardware statistics. */
	struct {
		uint64_t	tx_bytes;
		uint32_t	tx_frames;
		uint32_t	tx_frames_bcast;
		uint32_t	tx_frames_multi;
		uint32_t	tx_frames_pause;
		uint32_t	tx_frames_64b;
		uint32_t	tx_frames_65to127b;
		uint32_t	tx_frames_128to255b;
		uint32_t	tx_frames_256to511b;
		uint32_t	tx_frames_512to1023b;
		uint32_t	tx_frames_1024to1536b;
		uint32_t	tx_under_runs;
		uint32_t	tx_single_collisn;
		uint32_t	tx_multi_collisn;
		uint32_t	tx_excsv_collisn;
		uint32_t	tx_late_collisn;
		uint32_t	tx_deferred_frames;
		uint32_t	tx_carrier_sense_errs;
		uint64_t	rx_bytes;
		uint32_t	rx_frames;
		uint32_t	rx_frames_bcast;
		uint32_t	rx_frames_multi;
		uint32_t	rx_frames_pause;
		uint32_t	rx_frames_64b;
		uint32_t	rx_frames_65to127b;
		uint32_t	rx_frames_128to255b;
		uint32_t	rx_frames_256to511b;
		uint32_t	rx_frames_512to1023b;
		uint32_t	rx_frames_1024to1536b;
		uint32_t	rx_frames_undersize;
		uint32_t	rx_frames_oversize;
		uint32_t	rx_frames_jabber;
		uint32_t	rx_frames_fcs_errs;
		uint32_t	rx_frames_length_errs;
		uint32_t	rx_symbol_errs;
		uint32_t	rx_align_errs;
		uint32_t	rx_resource_errs;
		uint32_t	rx_overrun_errs;
		uint32_t	rx_ip_hdr_csum_errs;
		uint32_t	rx_tcp_csum_errs;
		uint32_t	rx_udp_csum_errs;
	} stats;

	/* Sysctl. */
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;
};

/* -----------------------------------------------------------------------
 * Register access — direct KVA reads/writes (same as arm64 bus_space
 * does internally for device memory; no struct resource needed).
 * ----------------------------------------------------------------------- */
#define RD4(sc, off) \
	(*(volatile uint32_t *)((char *)(sc)->mac_kva + (off)))
#define WR4(sc, off, val) \
	(*(volatile uint32_t *)((char *)(sc)->mac_kva + (off)) = (val))

/* Lock helpers (same names as cgem for minimal diff). */
#define CGEM_LOCK(sc)		mtx_lock(&(sc)->sc_mtx)
#define CGEM_UNLOCK(sc)		mtx_unlock(&(sc)->sc_mtx)
#define CGEM_LOCK_ASSERT(sc)	mtx_assert(&(sc)->sc_mtx, MA_OWNED)
#define CGEM_ASSERT_LOCKED(sc)	mtx_assert(&(sc)->sc_mtx, MA_OWNED)

/* Module-global pointer used by rp1eth_detach(). */
static struct rp1eth_softc *rp1eth_mac_sc;

/* Forward declarations. */
static void rp1eth_tick(void *);
static void cgem_recv(struct rp1eth_softc *);
static void cgem_clean_tx(struct rp1eth_softc *);
static void cgem_start_locked(if_t);
static void cgem_stop(struct rp1eth_softc *);
static void cgem_config(struct rp1eth_softc *);
static void cgem_init_locked(struct rp1eth_softc *);
static void cgem_reset(struct rp1eth_softc *);
static void cgem_fill_rqueue(struct rp1eth_softc *);
static void cgem_poll_hw_stats(struct rp1eth_softc *);

/* -----------------------------------------------------------------------
 * Multicast hash (verbatim from cgem)
 * ----------------------------------------------------------------------- */
static int
cgem_mac_hash(u_char eaddr[])
{
	int hash, i, j;

	hash = 0;
	for (i = 0; i < 6; i++)
		for (j = i; j < 48; j += 6)
			if ((eaddr[j >> 3] & (1 << (j & 7))) != 0)
				hash ^= (1 << i);
	return (hash);
}

static u_int
cgem_hash_maddr(void *arg, struct sockaddr_dl *sdl, u_int cnt)
{
	uint32_t *hashes = arg;
	int index;

	index = cgem_mac_hash(LLADDR(sdl));
	if (index > 31)
		hashes[0] |= (1U << (index - 32));
	else
		hashes[1] |= (1U << index);
	return (1);
}

/* -----------------------------------------------------------------------
 * RX filter — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_rx_filter(struct rp1eth_softc *sc)
{
	if_t ifp = sc->ifp;
	uint32_t hashes[2] = { 0, 0 };

	sc->net_cfg_shadow &= ~(CGEM_NET_CFG_MULTI_HASH_EN |
	    CGEM_NET_CFG_NO_BCAST | CGEM_NET_CFG_COPY_ALL);

	if ((if_getflags(ifp) & IFF_PROMISC) != 0)
		sc->net_cfg_shadow |= CGEM_NET_CFG_COPY_ALL;
	else {
		if ((if_getflags(ifp) & IFF_BROADCAST) == 0)
			sc->net_cfg_shadow |= CGEM_NET_CFG_NO_BCAST;
		if ((if_getflags(ifp) & IFF_ALLMULTI) != 0) {
			hashes[0] = 0xffffffff;
			hashes[1] = 0xffffffff;
		} else
			if_foreach_llmaddr(ifp, cgem_hash_maddr, hashes);

		if (hashes[0] != 0 || hashes[1] != 0)
			sc->net_cfg_shadow |= CGEM_NET_CFG_MULTI_HASH_EN;
	}

	WR4(sc, CGEM_HASH_TOP, hashes[0]);
	WR4(sc, CGEM_HASH_BOT, hashes[1]);
	WR4(sc, CGEM_NET_CFG, sc->net_cfg_shadow);
}

/* -----------------------------------------------------------------------
 * DMA map load callback — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_getaddr(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	if (nsegs != 1 || error != 0)
		return;
	*(bus_addr_t *)arg = segs[0].ds_addr;
}

/* -----------------------------------------------------------------------
 * Null queues for unused priority queues — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_null_qs(struct rp1eth_softc *sc)
{
	struct cgem_rx_desc *rx_desc;
	struct cgem_tx_desc *tx_desc;
	uint32_t queue_mask;
	int n;

	queue_mask = (RD4(sc, CGEM_DESIGN_CFG6) &
	    CGEM_DESIGN_CFG6_DMA_PRIO_Q_MASK) >> 1;
	if (queue_mask == 0)
		return;

	memset(sc->null_qs, 0, sizeof(struct cgem_rx_desc) +
	    sizeof(struct cgem_tx_desc));
	rx_desc = sc->null_qs;
	rx_desc->addr = CGEM_RXDESC_OWN | CGEM_RXDESC_WRAP;
	tx_desc = (struct cgem_tx_desc *)(rx_desc + 1);
	tx_desc->ctl = CGEM_TXDESC_USED | CGEM_TXDESC_WRAP;

	for (n = 1; (queue_mask & 1) != 0; n++, queue_mask >>= 1) {
		WR4(sc, CGEM_RX_QN_BAR(n), sc->null_qs_physaddr);
		WR4(sc, CGEM_TX_QN_BAR(n), sc->null_qs_physaddr +
		    sizeof(struct cgem_rx_desc));
	}
}

/* -----------------------------------------------------------------------
 * Descriptor ring setup — adapted from cgem:
 *   • parent DMA tag = NULL (root constraints)
 *   • lowaddr = BUS_SPACE_MAXADDR_32BIT (RP1 inbound window is 32-bit)
 *   • neednullqs always set (GEM_GXL has priority queues)
 * ----------------------------------------------------------------------- */
static int
cgem_setup_descs(struct rp1eth_softc *sc)
{
	int i, err;
	int desc_rings_size = RP1ETH_NUM_RX_DESCS * sizeof(struct cgem_rx_desc) +
	    RP1ETH_NUM_TX_DESCS * sizeof(struct cgem_tx_desc);

	/* null queues: one RX terminator + one TX terminator */
	desc_rings_size += sizeof(struct cgem_rx_desc) +
	    sizeof(struct cgem_tx_desc);

	sc->txring = NULL;
	sc->rxring = NULL;

	/*
	 * Descriptor DMA tag: 32-bit address space only.
	 * RP1 PCIe2 inbound window maps BCM2712 DRAM at 32-bit addresses.
	 */
	err = bus_dma_tag_create(NULL, 1, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    desc_rings_size, 1, desc_rings_size, 0,
	    busdma_lock_mutex, &sc->sc_mtx, &sc->desc_dma_tag);
	if (err)
		return (err);

	/* Mbuf DMA tag: same 32-bit constraint. */
	err = bus_dma_tag_create(NULL, 1, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    MCLBYTES, TX_MAX_DMA_SEGS, MCLBYTES, 0,
	    busdma_lock_mutex, &sc->sc_mtx, &sc->mbuf_dma_tag);
	if (err)
		return (err);

	/* Allocate coherent DMA memory for all descriptor rings at once. */
	err = bus_dmamem_alloc(sc->desc_dma_tag, (void **)&sc->rxring,
	    BUS_DMA_NOWAIT | BUS_DMA_COHERENT | BUS_DMA_ZERO,
	    &sc->rxring_dma_map);
	if (err)
		return (err);

	err = bus_dmamap_load(sc->desc_dma_tag, sc->rxring_dma_map,
	    (void *)sc->rxring, desc_rings_size,
	    cgem_getaddr, &sc->rxring_physaddr, BUS_DMA_NOWAIT);
	if (err)
		return (err);

	/* Verify 32-bit constraint was met. */
	if (sc->rxring_physaddr > 0xffffffffUL) {
		printf("rp1_eth: descriptor ring physaddr 0x%lx > 32-bit!\n",
		    (unsigned long)sc->rxring_physaddr);
		return (ENOMEM);
	}

	/* Initialize RX descriptors. */
	for (i = 0; i < RP1ETH_NUM_RX_DESCS; i++) {
		sc->rxring[i].addr = CGEM_RXDESC_OWN;
		sc->rxring[i].ctl = 0;
		sc->rxring_m[i] = NULL;
		sc->rxring_m_dmamap[i] = NULL;
	}
	sc->rxring[RP1ETH_NUM_RX_DESCS - 1].addr |= CGEM_RXDESC_WRAP;
	sc->rxring_hd_ptr = 0;
	sc->rxring_tl_ptr = 0;
	sc->rxring_queued = 0;

	/* TX ring follows RX ring in the same allocation. */
	sc->txring = (struct cgem_tx_desc *)(sc->rxring + RP1ETH_NUM_RX_DESCS);
	sc->txring_physaddr = sc->rxring_physaddr +
	    RP1ETH_NUM_RX_DESCS * sizeof(struct cgem_rx_desc);

	for (i = 0; i < RP1ETH_NUM_TX_DESCS; i++) {
		sc->txring[i].addr = 0;
		sc->txring[i].ctl = CGEM_TXDESC_USED;
		sc->txring_m[i] = NULL;
		sc->txring_m_dmamap[i] = NULL;
	}
	sc->txring[RP1ETH_NUM_TX_DESCS - 1].ctl |= CGEM_TXDESC_WRAP;
	sc->txring_hd_ptr = 0;
	sc->txring_tl_ptr = 0;
	sc->txring_queued = 0;

	/* Null queues for unused priority rings. */
	sc->null_qs = (void *)(sc->txring + RP1ETH_NUM_TX_DESCS);
	sc->null_qs_physaddr = sc->txring_physaddr +
	    RP1ETH_NUM_TX_DESCS * sizeof(struct cgem_tx_desc);
	cgem_null_qs(sc);

	return (0);
}

/* -----------------------------------------------------------------------
 * Fill receive ring — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_fill_rqueue(struct rp1eth_softc *sc)
{
	struct mbuf *m = NULL;
	bus_dma_segment_t segs[TX_MAX_DMA_SEGS];
	int nsegs;

	CGEM_ASSERT_LOCKED(sc);

	while (sc->rxring_queued < sc->rxbufs) {
		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL)
			break;

		m->m_len = MCLBYTES;
		m->m_pkthdr.len = MCLBYTES;
		m->m_pkthdr.rcvif = sc->ifp;

		if (bus_dmamap_create(sc->mbuf_dma_tag, 0,
		    &sc->rxring_m_dmamap[sc->rxring_hd_ptr])) {
			sc->rxdmamapfails++;
			m_free(m);
			break;
		}
		if (bus_dmamap_load_mbuf_sg(sc->mbuf_dma_tag,
		    sc->rxring_m_dmamap[sc->rxring_hd_ptr], m,
		    segs, &nsegs, BUS_DMA_NOWAIT)) {
			sc->rxdmamapfails++;
			bus_dmamap_destroy(sc->mbuf_dma_tag,
			    sc->rxring_m_dmamap[sc->rxring_hd_ptr]);
			sc->rxring_m_dmamap[sc->rxring_hd_ptr] = NULL;
			m_free(m);
			break;
		}
		sc->rxring_m[sc->rxring_hd_ptr] = m;

		bus_dmamap_sync(sc->mbuf_dma_tag,
		    sc->rxring_m_dmamap[sc->rxring_hd_ptr],
		    BUS_DMASYNC_PREREAD);

		sc->rxring[sc->rxring_hd_ptr].ctl = 0;
		if (sc->rxring_hd_ptr == RP1ETH_NUM_RX_DESCS - 1) {
			sc->rxring[sc->rxring_hd_ptr].addr =
			    segs[0].ds_addr | CGEM_RXDESC_WRAP;
			sc->rxring_hd_ptr = 0;
		} else
			sc->rxring[sc->rxring_hd_ptr++].addr = segs[0].ds_addr;

		sc->rxring_queued++;
	}
}

/* -----------------------------------------------------------------------
 * Receive — verbatim from cgem (device_printf → printf)
 * ----------------------------------------------------------------------- */
static void
cgem_recv(struct rp1eth_softc *sc)
{
	if_t ifp = sc->ifp;
	struct mbuf *m, *m_hd, **m_tl;
	uint32_t ctl;

	CGEM_ASSERT_LOCKED(sc);

	m_hd = NULL;
	m_tl = &m_hd;
	while (sc->rxring_queued > 0 &&
	    (sc->rxring[sc->rxring_tl_ptr].addr & CGEM_RXDESC_OWN) != 0) {
		ctl = sc->rxring[sc->rxring_tl_ptr].ctl;

		m = sc->rxring_m[sc->rxring_tl_ptr];
		sc->rxring_m[sc->rxring_tl_ptr] = NULL;

		bus_dmamap_sync(sc->mbuf_dma_tag,
		    sc->rxring_m_dmamap[sc->rxring_tl_ptr],
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->mbuf_dma_tag,
		    sc->rxring_m_dmamap[sc->rxring_tl_ptr]);
		bus_dmamap_destroy(sc->mbuf_dma_tag,
		    sc->rxring_m_dmamap[sc->rxring_tl_ptr]);
		sc->rxring_m_dmamap[sc->rxring_tl_ptr] = NULL;

		if (++sc->rxring_tl_ptr == RP1ETH_NUM_RX_DESCS)
			sc->rxring_tl_ptr = 0;
		sc->rxring_queued--;

		if ((ctl & CGEM_RXDESC_BAD_FCS) != 0 ||
		    (ctl & (CGEM_RXDESC_SOF | CGEM_RXDESC_EOF)) !=
		    (CGEM_RXDESC_SOF | CGEM_RXDESC_EOF)) {
			m_free(m);
			if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
			continue;
		}

		m->m_data += ETHER_ALIGN;
		m->m_len = (ctl & CGEM_RXDESC_LENGTH_MASK);
		m->m_pkthdr.rcvif = ifp;
		m->m_pkthdr.len = m->m_len;

		if ((if_getcapenable(ifp) & IFCAP_RXCSUM) != 0) {
			if ((ctl & CGEM_RXDESC_CKSUM_STAT_MASK) ==
			    CGEM_RXDESC_CKSUM_STAT_TCP_GOOD ||
			    (ctl & CGEM_RXDESC_CKSUM_STAT_MASK) ==
			    CGEM_RXDESC_CKSUM_STAT_UDP_GOOD) {
				m->m_pkthdr.csum_flags |=
				    CSUM_IP_CHECKED | CSUM_IP_VALID |
				    CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
				m->m_pkthdr.csum_data = 0xffff;
			} else if ((ctl & CGEM_RXDESC_CKSUM_STAT_MASK) ==
			    CGEM_RXDESC_CKSUM_STAT_IP_GOOD) {
				m->m_pkthdr.csum_flags |=
				    CSUM_IP_CHECKED | CSUM_IP_VALID;
				m->m_pkthdr.csum_data = 0xffff;
			}
		}

		*m_tl = m;
		m_tl = &m->m_next;
	}

	cgem_fill_rqueue(sc);

	CGEM_UNLOCK(sc);
	while (m_hd != NULL) {
		m = m_hd;
		m_hd = m_hd->m_next;
		m->m_next = NULL;
		if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
		if_input(ifp, m);
	}
	CGEM_LOCK(sc);
}

/* -----------------------------------------------------------------------
 * TX reclaim — verbatim from cgem (device_printf → printf)
 * ----------------------------------------------------------------------- */
static void
cgem_clean_tx(struct rp1eth_softc *sc)
{
	struct mbuf *m;
	uint32_t ctl;

	CGEM_ASSERT_LOCKED(sc);

	while (sc->txring_queued > 0 &&
	    ((ctl = sc->txring[sc->txring_tl_ptr].ctl) &
	    CGEM_TXDESC_USED) != 0) {
		bus_dmamap_sync(sc->mbuf_dma_tag,
		    sc->txring_m_dmamap[sc->txring_tl_ptr],
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->mbuf_dma_tag,
		    sc->txring_m_dmamap[sc->txring_tl_ptr]);
		bus_dmamap_destroy(sc->mbuf_dma_tag,
		    sc->txring_m_dmamap[sc->txring_tl_ptr]);
		sc->txring_m_dmamap[sc->txring_tl_ptr] = NULL;

		m = sc->txring_m[sc->txring_tl_ptr];
		sc->txring_m[sc->txring_tl_ptr] = NULL;
		m_freem(m);

		if ((ctl & CGEM_TXDESC_AHB_ERR) != 0) {
			printf("rp1_eth: TX AHB error, addr=0x%08x\n",
			    sc->txring[sc->txring_tl_ptr].addr);
		} else if ((ctl & (CGEM_TXDESC_RETRY_ERR |
		    CGEM_TXDESC_LATE_COLL)) != 0) {
			if_inc_counter(sc->ifp, IFCOUNTER_OERRORS, 1);
		} else
			if_inc_counter(sc->ifp, IFCOUNTER_OPACKETS, 1);

		while ((ctl & CGEM_TXDESC_LAST_BUF) == 0) {
			if ((ctl & CGEM_TXDESC_WRAP) != 0)
				sc->txring_tl_ptr = 0;
			else
				sc->txring_tl_ptr++;
			sc->txring_queued--;

			ctl = sc->txring[sc->txring_tl_ptr].ctl;
			sc->txring[sc->txring_tl_ptr].ctl =
			    ctl | CGEM_TXDESC_USED;
		}

		if ((ctl & CGEM_TXDESC_WRAP) != 0)
			sc->txring_tl_ptr = 0;
		else
			sc->txring_tl_ptr++;
		sc->txring_queued--;

		if_setdrvflagbits(sc->ifp, 0, IFF_DRV_OACTIVE);
	}
}

/* -----------------------------------------------------------------------
 * Transmit — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_start_locked(if_t ifp)
{
	struct rp1eth_softc *sc = (struct rp1eth_softc *)if_getsoftc(ifp);
	struct mbuf *m;
	bus_dma_segment_t segs[TX_MAX_DMA_SEGS];
	uint32_t ctl;
	int i, nsegs, wrap, err;

	CGEM_ASSERT_LOCKED(sc);

	if ((if_getdrvflags(ifp) & IFF_DRV_OACTIVE) != 0)
		return;

	for (;;) {
		if (sc->txring_queued >=
		    RP1ETH_NUM_TX_DESCS - TX_MAX_DMA_SEGS * 2) {
			cgem_clean_tx(sc);
			if (sc->txring_queued >=
			    RP1ETH_NUM_TX_DESCS - TX_MAX_DMA_SEGS * 2) {
				if_setdrvflagbits(ifp, IFF_DRV_OACTIVE, 0);
				sc->txfull++;
				break;
			}
		}

		m = if_dequeue(ifp);
		if (m == NULL)
			break;

		if (bus_dmamap_create(sc->mbuf_dma_tag, 0,
		    &sc->txring_m_dmamap[sc->txring_hd_ptr])) {
			m_freem(m);
			sc->txdmamapfails++;
			continue;
		}
		err = bus_dmamap_load_mbuf_sg(sc->mbuf_dma_tag,
		    sc->txring_m_dmamap[sc->txring_hd_ptr], m, segs, &nsegs,
		    BUS_DMA_NOWAIT);
		if (err == EFBIG) {
			struct mbuf *m2 = m_defrag(m, M_NOWAIT);
			if (m2 == NULL) {
				sc->txdefragfails++;
				m_freem(m);
				bus_dmamap_destroy(sc->mbuf_dma_tag,
				    sc->txring_m_dmamap[sc->txring_hd_ptr]);
				sc->txring_m_dmamap[sc->txring_hd_ptr] = NULL;
				continue;
			}
			m = m2;
			err = bus_dmamap_load_mbuf_sg(sc->mbuf_dma_tag,
			    sc->txring_m_dmamap[sc->txring_hd_ptr], m, segs,
			    &nsegs, BUS_DMA_NOWAIT);
			sc->txdefrags++;
		}
		if (err) {
			m_freem(m);
			bus_dmamap_destroy(sc->mbuf_dma_tag,
			    sc->txring_m_dmamap[sc->txring_hd_ptr]);
			sc->txring_m_dmamap[sc->txring_hd_ptr] = NULL;
			sc->txdmamapfails++;
			continue;
		}
		sc->txring_m[sc->txring_hd_ptr] = m;

		bus_dmamap_sync(sc->mbuf_dma_tag,
		    sc->txring_m_dmamap[sc->txring_hd_ptr],
		    BUS_DMASYNC_PREWRITE);

		wrap = sc->txring_hd_ptr + nsegs + TX_MAX_DMA_SEGS >=
		    RP1ETH_NUM_TX_DESCS;

		for (i = nsegs - 1; i >= 0; i--) {
			sc->txring[sc->txring_hd_ptr + i].addr =
			    segs[i].ds_addr;
			ctl = segs[i].ds_len;
			if (i == nsegs - 1) {
				ctl |= CGEM_TXDESC_LAST_BUF;
				if (wrap)
					ctl |= CGEM_TXDESC_WRAP;
			}
			sc->txring[sc->txring_hd_ptr + i].ctl = ctl;
			if (i != 0)
				sc->txring_m[sc->txring_hd_ptr + i] = NULL;
		}

		if (wrap)
			sc->txring_hd_ptr = 0;
		else
			sc->txring_hd_ptr += nsegs;
		sc->txring_queued += nsegs;

		WR4(sc, CGEM_NET_CTRL, sc->net_ctl_shadow |
		    CGEM_NET_CTRL_START_TX);

		ETHER_BPF_MTAP(ifp, m);
	}
}

static void
cgem_start(if_t ifp)
{
	struct rp1eth_softc *sc = (struct rp1eth_softc *)if_getsoftc(ifp);

	CGEM_LOCK(sc);
	cgem_start_locked(ifp);
	CGEM_UNLOCK(sc);
}

/* -----------------------------------------------------------------------
 * Hardware statistics poll — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_poll_hw_stats(struct rp1eth_softc *sc)
{
	uint32_t n;

	CGEM_ASSERT_LOCKED(sc);

	sc->stats.tx_bytes += RD4(sc, CGEM_OCTETS_TX_BOT);
	sc->stats.tx_bytes += (uint64_t)RD4(sc, CGEM_OCTETS_TX_TOP) << 32;
	sc->stats.tx_frames += RD4(sc, CGEM_FRAMES_TX);
	sc->stats.tx_frames_bcast += RD4(sc, CGEM_BCAST_FRAMES_TX);
	sc->stats.tx_frames_multi += RD4(sc, CGEM_MULTI_FRAMES_TX);
	sc->stats.tx_frames_pause += RD4(sc, CGEM_PAUSE_FRAMES_TX);
	sc->stats.tx_frames_64b += RD4(sc, CGEM_FRAMES_64B_TX);
	sc->stats.tx_frames_65to127b += RD4(sc, CGEM_FRAMES_65_127B_TX);
	sc->stats.tx_frames_128to255b += RD4(sc, CGEM_FRAMES_128_255B_TX);
	sc->stats.tx_frames_256to511b += RD4(sc, CGEM_FRAMES_256_511B_TX);
	sc->stats.tx_frames_512to1023b += RD4(sc, CGEM_FRAMES_512_1023B_TX);
	sc->stats.tx_frames_1024to1536b += RD4(sc, CGEM_FRAMES_1024_1518B_TX);
	sc->stats.tx_under_runs += RD4(sc, CGEM_TX_UNDERRUNS);

	n = RD4(sc, CGEM_SINGLE_COLL_FRAMES);
	sc->stats.tx_single_collisn += n;
	if_inc_counter(sc->ifp, IFCOUNTER_COLLISIONS, n);
	n = RD4(sc, CGEM_MULTI_COLL_FRAMES);
	sc->stats.tx_multi_collisn += n;
	if_inc_counter(sc->ifp, IFCOUNTER_COLLISIONS, n);
	n = RD4(sc, CGEM_EXCESSIVE_COLL_FRAMES);
	sc->stats.tx_excsv_collisn += n;
	if_inc_counter(sc->ifp, IFCOUNTER_COLLISIONS, n);
	n = RD4(sc, CGEM_LATE_COLL);
	sc->stats.tx_late_collisn += n;
	if_inc_counter(sc->ifp, IFCOUNTER_COLLISIONS, n);

	sc->stats.tx_deferred_frames += RD4(sc, CGEM_DEFERRED_TX_FRAMES);
	sc->stats.tx_carrier_sense_errs += RD4(sc, CGEM_CARRIER_SENSE_ERRS);

	sc->stats.rx_bytes += RD4(sc, CGEM_OCTETS_RX_BOT);
	sc->stats.rx_bytes += (uint64_t)RD4(sc, CGEM_OCTETS_RX_TOP) << 32;
	sc->stats.rx_frames += RD4(sc, CGEM_FRAMES_RX);
	sc->stats.rx_frames_bcast += RD4(sc, CGEM_BCAST_FRAMES_RX);
	sc->stats.rx_frames_multi += RD4(sc, CGEM_MULTI_FRAMES_RX);
	sc->stats.rx_frames_pause += RD4(sc, CGEM_PAUSE_FRAMES_RX);
	sc->stats.rx_frames_64b += RD4(sc, CGEM_FRAMES_64B_RX);
	sc->stats.rx_frames_65to127b += RD4(sc, CGEM_FRAMES_65_127B_RX);
	sc->stats.rx_frames_128to255b += RD4(sc, CGEM_FRAMES_128_255B_RX);
	sc->stats.rx_frames_256to511b += RD4(sc, CGEM_FRAMES_256_511B_RX);
	sc->stats.rx_frames_512to1023b += RD4(sc, CGEM_FRAMES_512_1023B_RX);
	sc->stats.rx_frames_1024to1536b += RD4(sc, CGEM_FRAMES_1024_1518B_RX);
	sc->stats.rx_frames_undersize += RD4(sc, CGEM_UNDERSZ_RX);
	sc->stats.rx_frames_oversize += RD4(sc, CGEM_OVERSZ_RX);
	sc->stats.rx_frames_jabber += RD4(sc, CGEM_JABBERS_RX);
	sc->stats.rx_frames_fcs_errs += RD4(sc, CGEM_FCS_ERRS);
	sc->stats.rx_frames_length_errs += RD4(sc, CGEM_LENGTH_FIELD_ERRS);
	sc->stats.rx_symbol_errs += RD4(sc, CGEM_RX_SYMBOL_ERRS);
	sc->stats.rx_align_errs += RD4(sc, CGEM_ALIGN_ERRS);
	sc->stats.rx_resource_errs += RD4(sc, CGEM_RX_RESOURCE_ERRS);
	sc->stats.rx_overrun_errs += RD4(sc, CGEM_RX_OVERRUN_ERRS);
	sc->stats.rx_ip_hdr_csum_errs += RD4(sc, CGEM_IP_HDR_CKSUM_ERRS);
	sc->stats.rx_tcp_csum_errs += RD4(sc, CGEM_TCP_CKSUM_ERRS);
	sc->stats.rx_udp_csum_errs += RD4(sc, CGEM_UDP_CKSUM_ERRS);
}

/* -----------------------------------------------------------------------
 * MDIO read/write — extracted from cgem_miibus_readreg/writereg.
 * Called with lock held (DELAY is a busy-wait, safe under mutex).
 * ----------------------------------------------------------------------- */
static int
rp1eth_mdio_read(struct rp1eth_softc *sc, int phy, int reg)
{
	int tries;

	WR4(sc, CGEM_PHY_MAINT,
	    CGEM_PHY_MAINT_CLAUSE_22 | CGEM_PHY_MAINT_MUST_10 |
	    CGEM_PHY_MAINT_OP_READ |
	    (phy << CGEM_PHY_MAINT_PHY_ADDR_SHIFT) |
	    (reg << CGEM_PHY_MAINT_REG_ADDR_SHIFT));

	tries = 0;
	while ((RD4(sc, CGEM_NET_STAT) & CGEM_NET_STAT_PHY_MGMT_IDLE) == 0) {
		DELAY(5);
		if (++tries > 200) {
			printf("rp1_eth: MDIO read timeout reg %d\n", reg);
			return (-1);
		}
	}
	return (RD4(sc, CGEM_PHY_MAINT) & CGEM_PHY_MAINT_DATA_MASK);
}

/* -----------------------------------------------------------------------
 * Polled tick — replaces cgem_tick.
 *
 * Fires at RP1ETH_POLL_HZ (200 Hz) with sc_mtx held.
 * Every tick: service RX/TX descriptor rings.
 * Every RP1ETH_POLL_HZ ticks (~1 s): poll link via BMSR, update stats.
 * ----------------------------------------------------------------------- */
static void
rp1eth_tick(void *arg)
{
	struct rp1eth_softc *sc = (struct rp1eth_softc *)arg;
	int bmsr, link_up;

	CGEM_ASSERT_LOCKED(sc);

	if ((if_getdrvflags(sc->ifp) & IFF_DRV_RUNNING) == 0)
		goto reschedule;

	/* Service RX and TX rings. */
	cgem_recv(sc);		/* drops+reacquires lock around mbuf delivery */
	cgem_clean_tx(sc);
	if (!if_sendq_empty(sc->ifp))
		cgem_start_locked(sc->ifp);

	/* Per-second work. */
	if (++sc->poll_tick >= RP1ETH_POLL_HZ) {
		sc->poll_tick = 0;

		/* Poll link state via BMSR (read twice to latch latching bits). */
		bmsr = rp1eth_mdio_read(sc, sc->phy_addr, MII_BMSR);
		bmsr = rp1eth_mdio_read(sc, sc->phy_addr, MII_BMSR);
		link_up = (bmsr >= 0) && ((bmsr & BMSR_LINK) != 0);

		if (link_up != sc->link_up) {
			sc->link_up = link_up;
			if_link_state_change(sc->ifp,
			    link_up ? LINK_STATE_UP : LINK_STATE_DOWN);
			printf("rp1_eth: link %s\n",
			    link_up ? "UP" : "DOWN");
		}

		cgem_poll_hw_stats(sc);
	}

reschedule:
	callout_reset(&sc->tick_ch, hz / RP1ETH_POLL_HZ, rp1eth_tick, sc);
}

/* -----------------------------------------------------------------------
 * Interrupt handler body — kept for Milestone 3.
 * Not wired to any IRQ in polled mode.
 * ----------------------------------------------------------------------- */
static void __unused
cgem_intr(void *arg)
{
	struct rp1eth_softc *sc = (struct rp1eth_softc *)arg;
	if_t ifp = sc->ifp;
	uint32_t istatus;

	/* TODO: Milestone 3 — wire to bcm2712_pcie interrupt. */

	CGEM_LOCK(sc);

	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0) {
		CGEM_UNLOCK(sc);
		return;
	}

	istatus = RD4(sc, CGEM_INTR_STAT);
	WR4(sc, CGEM_INTR_STAT, istatus);

	if ((istatus & CGEM_INTR_RX_COMPLETE) != 0)
		cgem_recv(sc);
	cgem_clean_tx(sc);

	if ((istatus & CGEM_INTR_HRESP_NOT_OK) != 0) {
		printf("rp1_eth: hresp not OK! rx_status=0x%x\n",
		    RD4(sc, CGEM_RX_STAT));
		WR4(sc, CGEM_RX_STAT, CGEM_RX_STAT_HRESP_NOT_OK);
	}
	if ((istatus & CGEM_INTR_RX_OVERRUN) != 0) {
		WR4(sc, CGEM_RX_STAT, CGEM_RX_STAT_OVERRUN);
		sc->rxoverruns++;
	}
	if ((istatus & CGEM_INTR_RX_USED_READ) != 0) {
		WR4(sc, CGEM_NET_CTRL, sc->net_ctl_shadow |
		    CGEM_NET_CTRL_FLUSH_DPRAM_PKT);
		cgem_fill_rqueue(sc);
		sc->rxnobufs++;
	}
	if (!if_sendq_empty(ifp))
		cgem_start_locked(ifp);

	CGEM_UNLOCK(sc);
}

/* -----------------------------------------------------------------------
 * Reset — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_reset(struct rp1eth_softc *sc)
{
	CGEM_ASSERT_LOCKED(sc);

	switch (RD4(sc, CGEM_DESIGN_CFG1) &
	    CGEM_DESIGN_CFG1_DMA_BUS_WIDTH_MASK) {
	case CGEM_DESIGN_CFG1_DMA_BUS_WIDTH_64:
		sc->net_cfg_shadow = CGEM_NET_CFG_DBUS_WIDTH_64;
		break;
	case CGEM_DESIGN_CFG1_DMA_BUS_WIDTH_128:
		sc->net_cfg_shadow = CGEM_NET_CFG_DBUS_WIDTH_128;
		break;
	default:
		sc->net_cfg_shadow = CGEM_NET_CFG_DBUS_WIDTH_32;
	}

	WR4(sc, CGEM_NET_CTRL, 0);
	WR4(sc, CGEM_NET_CFG, sc->net_cfg_shadow);
	WR4(sc, CGEM_NET_CTRL, CGEM_NET_CTRL_CLR_STAT_REGS);
	WR4(sc, CGEM_TX_STAT, CGEM_TX_STAT_ALL);
	WR4(sc, CGEM_RX_STAT, CGEM_RX_STAT_ALL);
	WR4(sc, CGEM_INTR_DIS, CGEM_INTR_ALL);	/* all interrupts masked */
	WR4(sc, CGEM_HASH_BOT, 0);
	WR4(sc, CGEM_HASH_TOP, 0);
	WR4(sc, CGEM_TX_QBAR, 0);
	WR4(sc, CGEM_RX_QBAR, 0);

	sc->net_cfg_shadow |= CGEM_NET_CFG_MDC_CLK_DIV_96;
	WR4(sc, CGEM_NET_CFG, sc->net_cfg_shadow);

	sc->net_ctl_shadow = CGEM_NET_CTRL_MGMT_PORT_EN;
	WR4(sc, CGEM_NET_CTRL, sc->net_ctl_shadow);
}

/* -----------------------------------------------------------------------
 * Config — adapted from cgem:
 *   • hardcoded 1G full duplex (negotiated in M1)
 *   • no SGMII path
 *   • interrupts stay disabled (polled mode)
 * ----------------------------------------------------------------------- */
static void
cgem_config(struct rp1eth_softc *sc)
{
	if_t ifp = sc->ifp;
	uint32_t dma_cfg;
	u_char *eaddr = if_getlladdr(ifp);

	CGEM_ASSERT_LOCKED(sc);

	sc->net_cfg_shadow &= (CGEM_NET_CFG_MDC_CLK_DIV_MASK |
	    CGEM_NET_CFG_DBUS_WIDTH_MASK);
	sc->net_cfg_shadow |= (CGEM_NET_CFG_FCS_REMOVE |
	    CGEM_NET_CFG_RX_BUF_OFFSET(ETHER_ALIGN) |
	    CGEM_NET_CFG_GIGE_EN | CGEM_NET_CFG_1536RXEN |
	    CGEM_NET_CFG_FULL_DUPLEX | CGEM_NET_CFG_SPEED100);

	if ((if_getcapenable(ifp) & IFCAP_RXCSUM) != 0)
		sc->net_cfg_shadow |= CGEM_NET_CFG_RX_CHKSUM_OFFLD_EN;

	WR4(sc, CGEM_NET_CFG, sc->net_cfg_shadow);

	dma_cfg = CGEM_DMA_CFG_RX_BUF_SIZE(MCLBYTES) |
	    CGEM_DMA_CFG_RX_PKTBUF_MEMSZ_SEL_8K |
	    CGEM_DMA_CFG_TX_PKTBUF_MEMSZ_SEL |
	    CGEM_DMA_CFG_AHB_FIXED_BURST_LEN_16 |
	    CGEM_DMA_CFG_DISC_WHEN_NO_AHB;

	if ((if_getcapenable(ifp) & IFCAP_TXCSUM) != 0)
		dma_cfg |= CGEM_DMA_CFG_CHKSUM_GEN_OFFLOAD_EN;

	WR4(sc, CGEM_DMA_CFG, dma_cfg);

	WR4(sc, CGEM_RX_QBAR, (uint32_t)sc->rxring_physaddr);
	WR4(sc, CGEM_TX_QBAR, (uint32_t)sc->txring_physaddr);

	/* Enable RX and TX. */
	sc->net_ctl_shadow |= (CGEM_NET_CTRL_TX_EN | CGEM_NET_CTRL_RX_EN);
	WR4(sc, CGEM_NET_CTRL, sc->net_ctl_shadow);

	WR4(sc, CGEM_SPEC_ADDR_LOW(0), (eaddr[3] << 24) |
	    (eaddr[2] << 16) | (eaddr[1] << 8) | eaddr[0]);
	WR4(sc, CGEM_SPEC_ADDR_HI(0), (eaddr[5] << 8) | eaddr[4]);

	/* Polled mode: keep all GEM interrupts masked. */
	WR4(sc, CGEM_INTR_DIS, CGEM_INTR_ALL);
}

/* -----------------------------------------------------------------------
 * Init — adapted from cgem (no mii_mediachg; callout fires at RP1ETH_POLL_HZ)
 * ----------------------------------------------------------------------- */
static void
cgem_init_locked(struct rp1eth_softc *sc)
{
	CGEM_ASSERT_LOCKED(sc);

	if ((if_getdrvflags(sc->ifp) & IFF_DRV_RUNNING) != 0)
		return;

	cgem_config(sc);
	cgem_fill_rqueue(sc);

	if_setdrvflagbits(sc->ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);

	callout_reset(&sc->tick_ch, hz / RP1ETH_POLL_HZ, rp1eth_tick, sc);
}

static void
cgem_init(void *arg)
{
	struct rp1eth_softc *sc = (struct rp1eth_softc *)arg;

	CGEM_LOCK(sc);
	cgem_init_locked(sc);
	CGEM_UNLOCK(sc);
}

/* -----------------------------------------------------------------------
 * Stop — verbatim from cgem
 * ----------------------------------------------------------------------- */
static void
cgem_stop(struct rp1eth_softc *sc)
{
	int i;

	CGEM_ASSERT_LOCKED(sc);

	callout_stop(&sc->tick_ch);
	cgem_reset(sc);

	memset(sc->txring, 0,
	    RP1ETH_NUM_TX_DESCS * sizeof(struct cgem_tx_desc));
	for (i = 0; i < RP1ETH_NUM_TX_DESCS; i++) {
		sc->txring[i].ctl = CGEM_TXDESC_USED;
		if (sc->txring_m[i]) {
			bus_dmamap_unload(sc->mbuf_dma_tag,
			    sc->txring_m_dmamap[i]);
			bus_dmamap_destroy(sc->mbuf_dma_tag,
			    sc->txring_m_dmamap[i]);
			sc->txring_m_dmamap[i] = NULL;
			m_freem(sc->txring_m[i]);
			sc->txring_m[i] = NULL;
		}
	}
	sc->txring[RP1ETH_NUM_TX_DESCS - 1].ctl |= CGEM_TXDESC_WRAP;
	sc->txring_hd_ptr = 0;
	sc->txring_tl_ptr = 0;
	sc->txring_queued = 0;

	memset(sc->rxring, 0,
	    RP1ETH_NUM_RX_DESCS * sizeof(struct cgem_rx_desc));
	for (i = 0; i < RP1ETH_NUM_RX_DESCS; i++) {
		sc->rxring[i].addr = CGEM_RXDESC_OWN;
		if (sc->rxring_m[i]) {
			bus_dmamap_unload(sc->mbuf_dma_tag,
			    sc->rxring_m_dmamap[i]);
			bus_dmamap_destroy(sc->mbuf_dma_tag,
			    sc->rxring_m_dmamap[i]);
			sc->rxring_m_dmamap[i] = NULL;
			m_freem(sc->rxring_m[i]);
			sc->rxring_m[i] = NULL;
		}
	}
	sc->rxring[RP1ETH_NUM_RX_DESCS - 1].addr |= CGEM_RXDESC_WRAP;
	sc->rxring_hd_ptr = 0;
	sc->rxring_tl_ptr = 0;
	sc->rxring_queued = 0;
}

/* -----------------------------------------------------------------------
 * ioctl — adapted from cgem (no miibus; use sc->ifmedia directly)
 * ----------------------------------------------------------------------- */
static int
cgem_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct rp1eth_softc *sc = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0, mask;

	switch (cmd) {
	case SIOCSIFFLAGS:
		CGEM_LOCK(sc);
		if ((if_getflags(ifp) & IFF_UP) != 0) {
			if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) != 0) {
				if (((if_getflags(ifp) ^ sc->if_old_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) != 0)
					cgem_rx_filter(sc);
			} else {
				cgem_init_locked(sc);
			}
		} else if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) != 0) {
			if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
			cgem_stop(sc);
		}
		sc->if_old_flags = if_getflags(ifp);
		CGEM_UNLOCK(sc);
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) != 0) {
			CGEM_LOCK(sc);
			cgem_rx_filter(sc);
			CGEM_UNLOCK(sc);
		}
		break;

	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->ifmedia, cmd);
		break;

	case SIOCSIFCAP:
		CGEM_LOCK(sc);
		mask = if_getcapenable(ifp) ^ ifr->ifr_reqcap;

		if ((mask & IFCAP_TXCSUM) != 0) {
			if ((ifr->ifr_reqcap & IFCAP_TXCSUM) != 0) {
				if_setcapenablebit(ifp,
				    IFCAP_TXCSUM | IFCAP_TXCSUM_IPV6, 0);
				WR4(sc, CGEM_DMA_CFG,
				    RD4(sc, CGEM_DMA_CFG) |
				    CGEM_DMA_CFG_CHKSUM_GEN_OFFLOAD_EN);
			} else {
				if_setcapenablebit(ifp, 0,
				    IFCAP_TXCSUM | IFCAP_TXCSUM_IPV6);
				WR4(sc, CGEM_DMA_CFG,
				    RD4(sc, CGEM_DMA_CFG) &
				    ~CGEM_DMA_CFG_CHKSUM_GEN_OFFLOAD_EN);
			}
		}
		if ((mask & IFCAP_RXCSUM) != 0) {
			if ((ifr->ifr_reqcap & IFCAP_RXCSUM) != 0) {
				if_setcapenablebit(ifp,
				    IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6, 0);
				sc->net_cfg_shadow |=
				    CGEM_NET_CFG_RX_CHKSUM_OFFLD_EN;
				WR4(sc, CGEM_NET_CFG, sc->net_cfg_shadow);
			} else {
				if_setcapenablebit(ifp, 0,
				    IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6);
				sc->net_cfg_shadow &=
				    ~CGEM_NET_CFG_RX_CHKSUM_OFFLD_EN;
				WR4(sc, CGEM_NET_CFG, sc->net_cfg_shadow);
			}
		}
		if ((if_getcapenable(ifp) & (IFCAP_RXCSUM | IFCAP_TXCSUM)) ==
		    (IFCAP_RXCSUM | IFCAP_TXCSUM))
			if_setcapenablebit(ifp, IFCAP_VLAN_HWCSUM, 0);
		else
			if_setcapenablebit(ifp, 0, IFCAP_VLAN_HWCSUM);

		CGEM_UNLOCK(sc);
		break;

	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	return (error);
}

/* -----------------------------------------------------------------------
 * ifmedia callbacks — simple fixed-media implementation (no miibus)
 * ----------------------------------------------------------------------- */
static int
rp1eth_ifmedia_upd(if_t ifp)
{
	/* Media is hardcoded to whatever M1 autoneg resolved. */
	return (0);
}

static void
rp1eth_ifmedia_sts(if_t ifp, struct ifmediareq *ifmr)
{
	struct rp1eth_softc *sc = if_getsoftc(ifp);

	ifmr->ifm_status = IFM_AVALID;
	if (sc->link_up)
		ifmr->ifm_status |= IFM_ACTIVE;
	ifmr->ifm_active = IFM_ETHER | IFM_1000_T | IFM_FDX;
}

/* -----------------------------------------------------------------------
 * Sysctls — minimal set under hw.rp1_eth.mac
 * ----------------------------------------------------------------------- */
static void
rp1eth_add_sysctls(struct rp1eth_softc *sc, struct sysctl_oid *parent)
{
	struct sysctl_ctx_list *ctx = &sc->sysctl_ctx;
	struct sysctl_oid_list *child;
	struct sysctl_oid *tree;

	sysctl_ctx_init(ctx);

	tree = SYSCTL_ADD_NODE(ctx,
	    SYSCTL_CHILDREN(parent), OID_AUTO, "mac_drv",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "GEM MAC driver");
	child = SYSCTL_CHILDREN(tree);
	sc->sysctl_tree = tree;

	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "rxbufs", CTLFLAG_RW,
	    &sc->rxbufs, 0, "Number of receive buffers queued");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "_rxoverruns", CTLFLAG_RD,
	    &sc->rxoverruns, 0, "Receive overrun events");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "_rxnobufs", CTLFLAG_RD,
	    &sc->rxnobufs, 0, "Receive ring empty events");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "_rxdmamapfails", CTLFLAG_RD,
	    &sc->rxdmamapfails, 0, "Receive DMA map failures");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "_txfull", CTLFLAG_RD,
	    &sc->txfull, 0, "Transmit ring full events");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "_txdmamapfails", CTLFLAG_RD,
	    &sc->txdmamapfails, 0, "Transmit DMA map failures");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "_txdefrags", CTLFLAG_RD,
	    &sc->txdefrags, 0, "Transmit m_defrag() calls");

	tree = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, "stats",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "GEM hardware statistics");
	child = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_UQUAD(ctx, child, OID_AUTO, "tx_bytes", CTLFLAG_RD,
	    &sc->stats.tx_bytes, "Bytes transmitted");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "tx_frames", CTLFLAG_RD,
	    &sc->stats.tx_frames, 0, "Frames transmitted");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "tx_under_runs", CTLFLAG_RD,
	    &sc->stats.tx_under_runs, 0, "Transmit under-runs");
	SYSCTL_ADD_UQUAD(ctx, child, OID_AUTO, "rx_bytes", CTLFLAG_RD,
	    &sc->stats.rx_bytes, "Bytes received");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "rx_frames", CTLFLAG_RD,
	    &sc->stats.rx_frames, 0, "Frames received");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "rx_frames_fcs_errs", CTLFLAG_RD,
	    &sc->stats.rx_frames_fcs_errs, 0, "FCS errors");
	SYSCTL_ADD_UINT(ctx, child, OID_AUTO, "rx_overrun_errs", CTLFLAG_RD,
	    &sc->stats.rx_overrun_errs, 0, "Receive overrun errors");
}

/* -----------------------------------------------------------------------
 * Attach — called from rp1_eth_cfg.c:MOD_LOAD after M1 hardware setup.
 * ----------------------------------------------------------------------- */
int
rp1eth_attach(struct rp1_eth_softc *cfg_sc)
{
	struct rp1eth_softc *sc;
	if_t ifp;
	uint32_t design_cfg6;
	int err;

	sc = malloc(sizeof(*sc), M_RP1ETH, M_WAITOK | M_ZERO);
	sc->mac_kva = (vm_offset_t)cfg_sc->mac_kva;
	sc->phy_addr = (int)cfg_sc->phy_addr;
	memcpy(sc->macaddr, cfg_sc->mac_addr, ETHER_ADDR_LEN);
	sc->rxbufs = RP1ETH_DEFAULT_RXBUFS;

	mtx_init(&sc->sc_mtx, "rp1eth", MTX_NETWORK_LOCK, MTX_DEF);

	/*
	 * Log the GEM hardware identity registers before touching anything.
	 * This is the first sanity check: if these return 0 or ~0, the PCIe
	 * mapping is wrong.
	 */
	design_cfg6 = RD4(sc, CGEM_DESIGN_CFG6);
	printf("rp1_eth: GEM DESIGN_CFG6=0x%08x MODULE_ID=0x%08x\n",
	    design_cfg6, RD4(sc, CGEM_MODULE_ID));
	printf("rp1_eth: GEM NET_CTRL=0x%08x NET_CFG=0x%08x NET_STAT=0x%08x\n",
	    RD4(sc, CGEM_NET_CTRL), RD4(sc, CGEM_NET_CFG),
	    RD4(sc, CGEM_NET_STAT));

	/*
	 * neednullqs: check DESIGN_CFG6 for priority queues.
	 * Set unconditionally — GEM_GXL on RP1 has priority queues.
	 */
	sc->neednullqs = 1;

	CGEM_LOCK(sc);
	cgem_reset(sc);
	CGEM_UNLOCK(sc);

	err = cgem_setup_descs(sc);
	if (err) {
		printf("rp1_eth: descriptor ring setup failed: %d\n", err);
		goto fail_descs;
	}

	printf("rp1_eth: RX ring physaddr=0x%08llx TX ring physaddr=0x%08llx\n",
	    (unsigned long long)sc->rxring_physaddr,
	    (unsigned long long)sc->txring_physaddr);

	/* Set up ifnet. */
	ifp = sc->ifp = if_alloc(IFT_ETHER);
	if_setsoftc(ifp, sc);
	if_initname(ifp, "rp1eth", 0);
	if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_setinitfn(ifp, cgem_init);
	if_setioctlfn(ifp, cgem_ioctl);
	if_setstartfn(ifp, cgem_start);
	if_setcapabilitiesbit(ifp, IFCAP_VLAN_MTU, 0);
	if_setsendqlen(ifp, RP1ETH_NUM_TX_DESCS);
	if_setsendqready(ifp);
	if_sethwassist(ifp, 0);
	if_setcapenable(ifp, if_getcapabilities(ifp));
	sc->if_old_flags = if_getflags(ifp);

	/* ifmedia: fixed 1G FDX (M1 autoneg result). */
	ifmedia_init(&sc->ifmedia, 0, rp1eth_ifmedia_upd, rp1eth_ifmedia_sts);
	ifmedia_add(&sc->ifmedia, IFM_ETHER | IFM_1000_T | IFM_FDX, 0, NULL);
	ifmedia_add(&sc->ifmedia, IFM_ETHER | IFM_100_TX | IFM_FDX, 0, NULL);
	ifmedia_add(&sc->ifmedia, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->ifmedia, IFM_ETHER | IFM_AUTO);

	callout_init_mtx(&sc->tick_ch, &sc->sc_mtx, 0);

	ether_ifattach(ifp, sc->macaddr);

	rp1eth_add_sysctls(sc, cfg_sc->sysctl_tree);

	rp1eth_mac_sc = sc;
	printf("rp1_eth: Milestone 2 attached — ifconfig rp1eth0\n");
	return (0);

fail_descs:
	if (sc->rxring != NULL) {
		if (sc->rxring_physaddr != 0)
			bus_dmamap_unload(sc->desc_dma_tag, sc->rxring_dma_map);
		bus_dmamem_free(sc->desc_dma_tag, sc->rxring,
		    sc->rxring_dma_map);
	}
	if (sc->desc_dma_tag != NULL)
		bus_dma_tag_destroy(sc->desc_dma_tag);
	if (sc->mbuf_dma_tag != NULL)
		bus_dma_tag_destroy(sc->mbuf_dma_tag);
	mtx_destroy(&sc->sc_mtx);
	free(sc, M_RP1ETH);
	return (err);
}

/* -----------------------------------------------------------------------
 * Detach — called from rp1_eth_cfg.c:MOD_UNLOAD
 * ----------------------------------------------------------------------- */
void
rp1eth_detach(void)
{
	struct rp1eth_softc *sc = rp1eth_mac_sc;
	int i;

	if (sc == NULL)
		return;
	rp1eth_mac_sc = NULL;

	CGEM_LOCK(sc);
	cgem_stop(sc);
	if_setdrvflagbits(sc->ifp, 0, IFF_DRV_RUNNING);
	CGEM_UNLOCK(sc);

	callout_drain(&sc->tick_ch);

	sysctl_ctx_free(&sc->sysctl_ctx);

	ifmedia_removeall(&sc->ifmedia);
	ether_ifdetach(sc->ifp);
	if_free(sc->ifp);

	/* Free DMA resources. */
	if (sc->rxring != NULL) {
		if (sc->rxring_physaddr != 0) {
			bus_dmamap_unload(sc->desc_dma_tag, sc->rxring_dma_map);
			sc->rxring_physaddr = 0;
			sc->txring_physaddr = 0;
			sc->null_qs_physaddr = 0;
		}
		bus_dmamem_free(sc->desc_dma_tag, sc->rxring,
		    sc->rxring_dma_map);
		sc->rxring = NULL;
		sc->txring = NULL;
		sc->null_qs = NULL;

		for (i = 0; i < RP1ETH_NUM_RX_DESCS; i++)
			if (sc->rxring_m_dmamap[i] != NULL) {
				bus_dmamap_destroy(sc->mbuf_dma_tag,
				    sc->rxring_m_dmamap[i]);
				sc->rxring_m_dmamap[i] = NULL;
			}
		for (i = 0; i < RP1ETH_NUM_TX_DESCS; i++)
			if (sc->txring_m_dmamap[i] != NULL) {
				bus_dmamap_destroy(sc->mbuf_dma_tag,
				    sc->txring_m_dmamap[i]);
				sc->txring_m_dmamap[i] = NULL;
			}
	}
	if (sc->desc_dma_tag != NULL) {
		bus_dma_tag_destroy(sc->desc_dma_tag);
		sc->desc_dma_tag = NULL;
	}
	if (sc->mbuf_dma_tag != NULL) {
		bus_dma_tag_destroy(sc->mbuf_dma_tag);
		sc->mbuf_dma_tag = NULL;
	}

	mtx_destroy(&sc->sc_mtx);
	free(sc, M_RP1ETH);
}
