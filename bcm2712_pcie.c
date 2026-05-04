/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * bcm2712_pcie — BCM2712 PCIe2→RP1 interrupt router (Milestone 3)
 *
 * Attaches to the "BCM2712" ACPI device added to the RP1B scope by the
 * DSDT override in /boot/acpi_dsdt.aml.  That device supplies two MMIO
 * resources (GEM MAC + eth_cfg) and the shared RP1 PCIe interrupt
 * (GIC SPI 229, ACPI GSI 261 = PINT from the RP1B scope).
 *
 * The interrupt is shared with xhci0/xhci1.  This driver acts as a
 * filter-only handler: it reads CGEM_INT_STATUS and dispatches to
 * rp1_eth's ISR if the GEM fired.
 *
 * KPI exported for rp1_eth:
 *   void bcm2712_pcie_register_rp1_intr(driver_filter_t *filter, void *arg)
 *   void bcm2712_pcie_deregister_rp1_intr(void)
 *
 * References:
 *   if_gem-PLAN.md §3.2
 *   sys/dev/cadence/if_cgem.c (CGEM_INT_STATUS definition)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>
#include <dev/acpica/acpivar.h>

#include "bcm2712_pcie.h"

/* GEM interrupt status register — checked in the filter to confirm GEM fired */
#define CGEM_INT_STATUS		0x024
#define CGEM_INT_RX_COMPLETE	(1u << 1)
#define CGEM_INT_RX_USED_READ	(1u << 2)
#define CGEM_INT_TX_COMPLETE	(1u << 7)
#define CGEM_INT_TX_USED_READ	(1u << 3)
#define CGEM_INT_HRESP_NOT_OK	(1u << 11)
#define CGEM_INT_RX_OVERRUN	(1u << 10)
#define CGEM_INT_ANY		(CGEM_INT_RX_COMPLETE | CGEM_INT_RX_USED_READ | \
				 CGEM_INT_TX_COMPLETE | CGEM_INT_TX_USED_READ | \
				 CGEM_INT_HRESP_NOT_OK | CGEM_INT_RX_OVERRUN)

/* RP1 PCIE_CFG registers for MSIx IACK re-arm (RP-008370-DS-1 ss6.2) */
#define PCIE_CFG_PHYS       0x1f00108000UL  /* BCM2712 CPU physical address */
#define PCIE_CFG_SIZE       0x200
#define PCIE_CFG_MSIX_CFG_0 0x008   /* MSIX_CFG_n base; vector n at offset +n*4 */
#define MSIX_CFG_IACK       (1u << 2) /* SC: write 1 to re-arm vector */
#define PCIE_CFG_INTSTATL   0x108   /* vectors 0-31 assertion status (RO) */
#define PCIE_CFG_INTSTATH   0x10c   /* vectors 32-63 assertion status (RO) */
#define RP1_INT_ETH          6      /* RP1 GEM MSI vector (INTSTATL bit 6 = 0x40, verified at runtime) */

struct bcm2712_pcie_softc {
	device_t	 dev;
	struct resource	*mac_res;	/* SYS_RES_MEMORY rid 0: GEM MAC */
	struct resource	*cfg_res;	/* SYS_RES_MEMORY rid 1: eth_cfg */
	struct resource	*irq_res;	/* SYS_RES_IRQ    rid 0: shared */
	void		*intr_cookie;
	bus_space_tag_t	 pciecfg_bst;
	bus_space_handle_t pciecfg_bsh;
	int		 pciecfg_mapped;
};

/*
 * Module-level callback storage for rp1_eth's interrupt filter.
 *
 * These are intentionally NOT in bcm2712_pcie_softc.  The rp1_eth module
 * is often loaded from the boot loader and calls
 * bcm2712_pcie_register_rp1_intr() before bcm2712_pcie0 has probed/attached
 * (ACPI runs after the early-KLD phase).  By storing the callback here, the
 * registration succeeds at any time, and the interrupt filter picks it up as
 * soon as bcm2712_pcie0 hooks the GIC line.
 *
 * Ordering contract (both store and load use rel/acq barriers):
 *   register:   store arg first, then filter (filter == NULL ⇒ arg ignored)
 *   deregister: clear filter first, then arg (filter == NULL ⇒ arg never read)
 */
static volatile uintptr_t g_rp1_filter;	/* atomic: driver_filter_t * */
static volatile uintptr_t g_rp1_arg;	/* atomic: void * */

/* Module-level PCIE_CFG handle for bcm2712_pcie_gem_iack() KPI */
static bus_space_tag_t    g_pciecfg_bst;
static bus_space_handle_t g_pciecfg_bsh;
static int                g_pciecfg_mapped;

/*
 * KPI: rp1_eth calls this to register its GEM interrupt filter.
 * Safe to call before or after bcm2712_pcie0 attaches.
 */
void
bcm2712_pcie_register_rp1_intr(driver_filter_t *filter, void *arg)
{
	/* Store arg before filter so the ISR never sees a stale arg. */
	atomic_store_rel_ptr(&g_rp1_arg, (uintptr_t)arg);
	atomic_store_rel_ptr(&g_rp1_filter, (uintptr_t)filter);
}

void
bcm2712_pcie_deregister_rp1_intr(void)
{
	/* Clear filter before arg so the ISR never fires with a stale arg. */
	atomic_store_rel_ptr(&g_rp1_filter, (uintptr_t)NULL);
	atomic_store_rel_ptr(&g_rp1_arg, (uintptr_t)NULL);
}

/*
 * Interrupt filter: called at interrupt level, no sleeping.
 * Read GEM INT_STATUS directly (the MAC resource is mapped at attach).
 * If GEM bits are set, dispatch to rp1_eth's filter.
 * Return FILTER_STRAY if GEM is not the source so xhci handlers run.
 */
static int
bcm2712_pcie_filter(void *arg)
{
	struct bcm2712_pcie_softc *sc = arg;
	driver_filter_t *filter;
	void *filter_arg;
	uint32_t istat;

	istat = bus_read_4(sc->mac_res, CGEM_INT_STATUS);
	if ((istat & CGEM_INT_ANY) == 0)
		return (FILTER_STRAY);

	filter = (driver_filter_t *)atomic_load_acq_ptr(&g_rp1_filter);
	if (filter == NULL)
		return (FILTER_STRAY);
	filter_arg = (void *)atomic_load_acq_ptr(&g_rp1_arg);

	/*
	 * Call GEM ISR first so it clears INT_STATUS.  Then write IACK to
	 * re-arm the RP1 MSIx vector (IACK_EN=1 is set by RP1 firmware).
	 * Writing IACK after INT_STATUS is clear means a new MSI fires only
	 * if a new packet arrived while we were handling this one -- correct.
	 * Writing before would re-arm while INT_STATUS still asserted,
	 * causing a spurious back-to-back interrupt.
	 */
	return (filter(filter_arg));
}

/*
 * KPI: called by rp1_eth cgem_intr_task after reading (clearing) GEM
 * INT_STATUS and re-enabling GEM interrupts.  At that point the GEM
 * interrupt source has de-asserted, so IACK re-arms the vector safely.
 * A new MSI fires only if a packet arrived after INT_STATUS was cleared.
 * MUST NOT be called from the filter -- INT_STATUS still set there causes
 * an immediate re-fire loop (interrupt storm).
 */
void
bcm2712_pcie_gem_iack(void)
{
	if (g_pciecfg_mapped)
		bus_space_write_4(g_pciecfg_bst, g_pciecfg_bsh,
		    PCIE_CFG_MSIX_CFG_0 + RP1_INT_ETH * 4, MSIX_CFG_IACK);
}

static int
bcm2712_pcie_probe(device_t dev)
{
	static char *ids[] = { "BCM2712", NULL };

	if (ACPI_ID_PROBE(device_get_parent(dev), dev, ids, NULL) > 0)
		return (ENXIO);
	device_set_desc(dev, "BCM2712 PCIe2/RP1 GEM interrupt router");
	return (BUS_PROBE_DEFAULT);
}

static int
bcm2712_pcie_attach(device_t dev)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	int rid, error;

	sc->dev = dev;

	/* Map GEM MAC registers (memory resource 0) */
	rid = 0;
	sc->mac_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mac_res == NULL) {
		device_printf(dev, "cannot map GEM MAC registers\n");
		return (ENXIO);
	}

	/* Map RP1 PCIE_CFG for MSIx IACK re-arm (direct physical map) */
	sc->pciecfg_bst = rman_get_bustag(sc->mac_res);
	if (bus_space_map(sc->pciecfg_bst, PCIE_CFG_PHYS, PCIE_CFG_SIZE, 0,
	    &sc->pciecfg_bsh) == 0) {
		sc->pciecfg_mapped = 1;
		g_pciecfg_bst = sc->pciecfg_bst;
		g_pciecfg_bsh = sc->pciecfg_bsh;
		g_pciecfg_mapped = 1;
	} else {
		device_printf(dev, "warning: cannot map PCIE_CFG, IACK disabled\n");
		sc->pciecfg_mapped = 0;
	}

	/* Map eth_cfg registers (memory resource 1) — optional for now */
	rid = 1;
	sc->cfg_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->cfg_res == NULL)
		device_printf(dev, "warning: cannot map eth_cfg registers\n");

	/* Allocate shared interrupt (GIC SPI 229 / GSI 261) */
	rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "cannot allocate interrupt\n");
		error = ENXIO;
		goto fail_mem;
	}

	error = bus_setup_intr(dev, sc->irq_res,
	    INTR_TYPE_NET | INTR_MPSAFE,
	    bcm2712_pcie_filter, NULL, sc, &sc->intr_cookie);
	if (error != 0) {
		device_printf(dev, "cannot set up interrupt: %d\n", error);
		goto fail_irq;
	}

	device_printf(dev, "GEM MAC mapped at %#jx, IRQ hooked (shared GIC SPI 229)\n",
	    (uintmax_t)rman_get_start(sc->mac_res));
	return (0);

fail_irq:
	bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
fail_mem:
	if (sc->cfg_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 1, sc->cfg_res);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mac_res);
	return (error);
}

static int
bcm2712_pcie_detach(device_t dev)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);

	bus_teardown_intr(dev, sc->irq_res, sc->intr_cookie);
	bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	if (sc->pciecfg_mapped)
		g_pciecfg_mapped = 0;
		bus_space_unmap(sc->pciecfg_bst, sc->pciecfg_bsh, PCIE_CFG_SIZE);
	if (sc->cfg_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 1, sc->cfg_res);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mac_res);
	return (0);
}

static device_method_t bcm2712_pcie_methods[] = {
	DEVMETHOD(device_probe,		bcm2712_pcie_probe),
	DEVMETHOD(device_attach,	bcm2712_pcie_attach),
	DEVMETHOD(device_detach,	bcm2712_pcie_detach),
	DEVMETHOD_END
};

static driver_t bcm2712_pcie_driver = {
	"bcm2712_pcie",
	bcm2712_pcie_methods,
	sizeof(struct bcm2712_pcie_softc),
};

DRIVER_MODULE(bcm2712_pcie, acpi, bcm2712_pcie_driver, NULL, NULL);
MODULE_VERSION(bcm2712_pcie, 1);
MODULE_DEPEND(bcm2712_pcie, acpi, 1, 1, 1);
