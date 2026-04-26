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
#include <sys/mutex.h>
#include <sys/lock.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/acpica/acpivar.h>

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

struct bcm2712_pcie_softc {
	device_t	 dev;
	struct resource	*mac_res;	/* SYS_RES_MEMORY rid 0: GEM MAC */
	struct resource	*cfg_res;	/* SYS_RES_MEMORY rid 1: eth_cfg */
	struct resource	*irq_res;	/* SYS_RES_IRQ    rid 0: shared */
	void		*intr_cookie;
	struct mtx	 child_mtx;
	driver_filter_t	*child_filter;
	void		*child_arg;
};

static struct bcm2712_pcie_softc *g_sc;	/* singleton for KPI access */

/*
 * KPI: rp1_eth calls this during attach to register its interrupt filter.
 * The filter must be self-contained (no sleeping), check GEM INT_STATUS,
 * and return FILTER_HANDLED if it consumed the interrupt.
 */
void
bcm2712_pcie_register_rp1_intr(driver_filter_t *filter, void *arg)
{
	if (g_sc == NULL)
		return;
	mtx_lock(&g_sc->child_mtx);
	g_sc->child_filter = filter;
	g_sc->child_arg    = arg;
	mtx_unlock(&g_sc->child_mtx);
}

void
bcm2712_pcie_deregister_rp1_intr(void)
{
	if (g_sc == NULL)
		return;
	mtx_lock(&g_sc->child_mtx);
	g_sc->child_filter = NULL;
	g_sc->child_arg    = NULL;
	mtx_unlock(&g_sc->child_mtx);
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
	uint32_t istat;
	int ret;

	istat = bus_read_4(sc->mac_res, CGEM_INT_STATUS);
	if ((istat & CGEM_INT_ANY) == 0)
		return (FILTER_STRAY);

	mtx_lock(&sc->child_mtx);
	if (sc->child_filter != NULL)
		ret = sc->child_filter(sc->child_arg);
	else
		ret = FILTER_STRAY;
	mtx_unlock(&sc->child_mtx);

	return (ret);
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
	mtx_init(&sc->child_mtx, "bcm2712_pcie", NULL, MTX_DEF);

	/* Map GEM MAC registers (memory resource 0) */
	rid = 0;
	sc->mac_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mac_res == NULL) {
		device_printf(dev, "cannot map GEM MAC registers\n");
		error = ENXIO;
		goto fail_mtx;
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

	g_sc = sc;
	device_printf(dev, "GEM MAC mapped at %#jx, IRQ hooked (shared GIC SPI 229)\n",
	    (uintmax_t)rman_get_start(sc->mac_res));
	return (0);

fail_irq:
	bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
fail_mem:
	if (sc->cfg_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 1, sc->cfg_res);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mac_res);
fail_mtx:
	mtx_destroy(&sc->child_mtx);
	return (error);
}

static int
bcm2712_pcie_detach(device_t dev)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);

	g_sc = NULL;
	bus_teardown_intr(dev, sc->irq_res, sc->intr_cookie);
	bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	if (sc->cfg_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 1, sc->cfg_res);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mac_res);
	mtx_destroy(&sc->child_mtx);
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
