/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * rp1_gpio — RP1 GPIO / Pinctrl Controller
 *
 * Milestone 1: device_identify probe, pmap_mapdev_attr register mapping,
 * gpio-line-names population, full gpio_if(9) implementation
 * (pin_get/set/toggle/flags/caps/name), fdt_pinctrl stub.
 *
 * Milestone 2: rp1_gpio_func.c function table + fdt_pinctrl configure_pins.
 * Milestone 3: PADS pull / drive-strength / schmitt in pin_setflags.
 * Milestone 4: pic_if per-pin edge/level interrupts.
 *
 * Attach strategy (M1 finding):
 *   The Pi 5 boots with ACPI; no simplebus enumerates FDT children.
 *   DRIVER_MODULE(nexus) + device_identify walks the FDT directly, creates
 *   a synthetic device_t, and maps registers via pmap_mapdev_attr — the
 *   same pattern used by bcm2712.c and rp1_eth_cfg.c in this repo.
 *   IRQ chain validation is deferred to M4; the ACPI-booted system will
 *   require a different interrupt delivery path than the FDT chain assumed
 *   in the original plan (see RP1_GPIO_spec.md §9.4).
 *
 * Pattern: sys/arm/broadcom/bcm2835/bcm2835_gpio.c
 * Hardware: RP-008370-DS-1 §3 (IO_BANK, SYS_RIO, PADS_BANK)
 * FDT node: /axi/pcie@1000120000/rp1/gpio@d0000
 *           compatible = "raspberrypi,rp1-gpio"
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>

#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>

#include "gpio_if.h"
#include "fdt_pinctrl_if.h"
#include "rp1_gpio_var.h"

/* -----------------------------------------------------------------------
 * FDT node location
 *
 * Try known Pi 5 paths first; fall back to compatible-string search.
 * ----------------------------------------------------------------------- */
static const char * const rp1_gpio_fdt_paths[] = {
	"/axi/pcie@1000120000/rp1/gpio@d0000",
	"/soc/rp1/gpio@d0000",
	NULL
};

static phandle_t
rp1_gpio_find_node(void)
{
	phandle_t node;
	int i;

	for (i = 0; rp1_gpio_fdt_paths[i] != NULL; i++) {
		node = OF_finddevice(rp1_gpio_fdt_paths[i]);
		if (node != -1 &&
		    ofw_bus_node_is_compatible(node, "raspberrypi,rp1-gpio"))
			return (node);
	}
	return (-1);
}

/* -----------------------------------------------------------------------
 * Pin name population from gpio-line-names DTB property
 * ----------------------------------------------------------------------- */
static void
rp1_gpio_parse_pin_names(struct rp1_gpio_softc *sc, phandle_t node)
{
	char *buf;
	const char *p, *end;
	ssize_t slen;
	int i;

	slen = OF_getprop_alloc(node, "gpio-line-names", (void **)&buf);
	if (slen > 0) {
		p = buf;
		end = buf + slen;
		for (i = 0; i < RP1_NUM_GPIOS; i++) {
			if (p >= end || *p == '\0' || strcmp(p, "-") == 0)
				snprintf(sc->sc_pins[i].gp_name, GPIOMAXNAME,
				    "pin%d", i);
			else
				strlcpy(sc->sc_pins[i].gp_name, p, GPIOMAXNAME);
			if (p < end)
				p += strnlen(p, end - p) + 1;
		}
		OF_prop_free(buf);
	} else {
		for (i = 0; i < RP1_NUM_GPIOS; i++)
			snprintf(sc->sc_pins[i].gp_name, GPIOMAXNAME, "pin%d", i);
	}
}

/* -----------------------------------------------------------------------
 * Identify / probe / attach / detach
 * ----------------------------------------------------------------------- */
static int rp1_gpio_detach(device_t dev);	/* forward declaration */

/*
 * identify: called by nexus bus_generic_probe.  Creates a device_t if the
 * gpio@d0000 FDT node is present and no instance already exists.
 */
static void
rp1_gpio_identify(driver_t *driver, device_t parent)
{
	if (rp1_gpio_find_node() == -1)
		return;
	if (device_find_child(parent, "rp1_gpio", -1) != NULL)
		return;
	if (BUS_ADD_CHILD(parent, 0, "rp1_gpio", -1) == NULL)
		device_printf(parent, "rp1_gpio: BUS_ADD_CHILD failed\n");
}

/*
 * probe: device was created by our identify method, so just set the
 * description and claim it.
 */
static int
rp1_gpio_probe(device_t dev)
{
	device_set_desc(dev, "RP1 GPIO / Pinctrl Controller");
	return (BUS_PROBE_DEFAULT);
}

static int
rp1_gpio_attach(device_t dev)
{
	struct rp1_gpio_softc *sc;
	phandle_t node;
	uint32_t ctrl, funcsel, oe;
	int bank, i;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_npins = RP1_NUM_GPIOS;

	node = rp1_gpio_find_node();
	if (node == -1) {
		device_printf(dev, "gpio@d0000 FDT node not found\n");
		return (ENXIO);
	}

	/*
	 * Map the three register windows.  Physical addresses are hard-coded
	 * from the DTB reg property (verified in RP1_GPIO_spec.md §3.1).
	 * pmap_mapdev_attr mirrors the approach in bcm2712.c and rp1_eth_cfg.c.
	 */
	sc->sc_io_kva = pmap_mapdev_attr(RP1_IO_BANK_BASE_PHYS,
	    RP1_GPIO_REGION_SIZE, VM_MEMATTR_DEVICE);
	if (sc->sc_io_kva == NULL) {
		device_printf(dev, "cannot map IO_BANK\n");
		return (ENOMEM);
	}
	sc->sc_rio_kva = pmap_mapdev_attr(RP1_SYS_RIO_BASE_PHYS,
	    RP1_GPIO_REGION_SIZE, VM_MEMATTR_DEVICE);
	if (sc->sc_rio_kva == NULL) {
		device_printf(dev, "cannot map SYS_RIO\n");
		pmap_unmapdev(sc->sc_io_kva, RP1_GPIO_REGION_SIZE);
		return (ENOMEM);
	}
	sc->sc_pad_kva = pmap_mapdev_attr(RP1_PADS_BANK_BASE_PHYS,
	    RP1_GPIO_REGION_SIZE, VM_MEMATTR_DEVICE);
	if (sc->sc_pad_kva == NULL) {
		device_printf(dev, "cannot map PADS_BANK\n");
		pmap_unmapdev(sc->sc_rio_kva, RP1_GPIO_REGION_SIZE);
		pmap_unmapdev(sc->sc_io_kva, RP1_GPIO_REGION_SIZE);
		return (ENOMEM);
	}

	mtx_init(&sc->sc_mtx, "rp1gpio", NULL, MTX_SPIN);

	/*
	 * M1 task 4 finding: this system is ACPI-booted; no FDT bus framework
	 * routes IRQs through the RP1 interrupt-controller node.  IRQ chain
	 * validation and per-pin interrupt delivery are deferred to M4, which
	 * will need a delivery path consistent with the ACPI device model
	 * (similar to bcm2712_pcie.c's approach for the GEM IRQ).
	 */
	device_printf(dev,
	    "IO_BANK@%p RIO@%p PADS@%p (IRQ chain: deferred to M4)\n",
	    sc->sc_io_kva, sc->sc_rio_kva, sc->sc_pad_kva);

	/* Populate pin table: names from gpio-line-names, flags from FUNCSEL/OE */
	rp1_gpio_parse_pin_names(sc, node);

	for (i = 0; i < RP1_NUM_GPIOS; i++) {
		sc->sc_pins[i].gp_pin = i;
		sc->sc_pins[i].gp_caps =
		    GPIO_PIN_INPUT  | GPIO_PIN_OUTPUT  |
		    GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN |
		    GPIO_PIN_INVIN  | GPIO_PIN_INVOUT;

		ctrl = rp1_io_read(sc, i);
		funcsel = ctrl & RP1_CTRL_FUNCSEL_MASK;
		if (funcsel == RP1_FSEL_GPIO) {
			bank = RP1_GPIO_BANK(i);
			oe = rp1_rio_read(sc, bank, RP1_RIO_RW, RP1_RIO_OE);
			sc->sc_pins[i].gp_flags =
			    (oe & (1u << RP1_GPIO_PIN_IN_BANK(i)))
			    ? GPIO_PIN_OUTPUT : GPIO_PIN_INPUT;
		} else {
			sc->sc_pins[i].gp_flags = 0;	/* peripheral */
		}
	}

	/*
	 * fdt_pinctrl registration requires ofw_bus_get_node(dev) to return a
	 * valid phandle.  For a synthetic nexus device that has no OFW bus
	 * ivars, this returns -1 and registration silently fails.  Defer to M2
	 * where the FDT/device-framework integration will be resolved.
	 */

	/*
	 * API note (16-CURRENT, post-commit 186100f13bd2):
	 * gpiobus_add_bus() only adds the "gpiobus" child; it no longer calls
	 * device_probe_and_attach() itself.  The GPIO controller is responsible
	 * for calling bus_attach_children(dev) to probe+attach gpiobus (and
	 * transitively gpioc) — the same pattern used by bcm_gpio_attach().
	 */
	sc->sc_busdev = gpiobus_add_bus(dev);
	if (sc->sc_busdev == NULL) {
		device_printf(dev, "cannot attach gpiobus\n");
		rp1_gpio_detach(dev);
		return (ENXIO);
	}

	bus_attach_children(dev);

	return (0);
}

static int
rp1_gpio_detach(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (sc->sc_busdev != NULL) {
		gpiobus_detach_bus(dev);
		sc->sc_busdev = NULL;
	}

	if (mtx_initialized(&sc->sc_mtx))
		mtx_destroy(&sc->sc_mtx);

	if (sc->sc_pad_kva != NULL) {
		pmap_unmapdev(sc->sc_pad_kva, RP1_GPIO_REGION_SIZE);
		sc->sc_pad_kva = NULL;
	}
	if (sc->sc_rio_kva != NULL) {
		pmap_unmapdev(sc->sc_rio_kva, RP1_GPIO_REGION_SIZE);
		sc->sc_rio_kva = NULL;
	}
	if (sc->sc_io_kva != NULL) {
		pmap_unmapdev(sc->sc_io_kva, RP1_GPIO_REGION_SIZE);
		sc->sc_io_kva = NULL;
	}

	return (0);
}

/* -----------------------------------------------------------------------
 * gpio_if(9) methods
 * ----------------------------------------------------------------------- */
static device_t
rp1_gpio_get_bus(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	return (sc->sc_busdev);
}

static int
rp1_gpio_pin_max(device_t dev, int *maxpin)
{
	*maxpin = RP1_NUM_GPIOS - 1;
	return (0);
}

static int
rp1_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (pin >= sc->sc_npins)
		return (EINVAL);
	strlcpy(name, sc->sc_pins[pin].gp_name, GPIOMAXNAME);
	return (0);
}

static int
rp1_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (pin >= sc->sc_npins)
		return (EINVAL);
	*caps = sc->sc_pins[pin].gp_caps;
	return (0);
}

static int
rp1_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	uint32_t ctrl, funcsel, oe;
	int bank;

	if (pin >= sc->sc_npins)
		return (EINVAL);

	RP1_GPIO_LOCK(sc);
	ctrl = rp1_io_read(sc, pin);
	funcsel = ctrl & RP1_CTRL_FUNCSEL_MASK;
	if (funcsel == RP1_FSEL_GPIO) {
		bank = RP1_GPIO_BANK(pin);
		oe = rp1_rio_read(sc, bank, RP1_RIO_RW, RP1_RIO_OE);
		*flags = (oe & (1u << RP1_GPIO_PIN_IN_BANK(pin)))
		    ? GPIO_PIN_OUTPUT : GPIO_PIN_INPUT;
	} else {
		*flags = 0;
	}
	sc->sc_pins[pin].gp_flags = *flags;
	RP1_GPIO_UNLOCK(sc);

	return (0);
}

/*
 * Switch a pin to software GPIO mode (FUNCSEL=5) and set its direction.
 * OUTOVER and OEOVER are cleared to PERI so the RIO block drives the pad.
 * Pull-up/pull-down flags are accepted but deferred to M3 (PADS writes).
 * IE is set in PADS so IN_SYNC reflects the pad voltage in both directions.
 */
static int
rp1_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	uint32_t ctrl, pad;
	uint32_t bit;
	int bank;

	if (pin >= sc->sc_npins)
		return (EINVAL);
	if ((flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)) == 0)
		return (0);

	bank = RP1_GPIO_BANK(pin);
	bit  = 1u << RP1_GPIO_PIN_IN_BANK(pin);

	RP1_GPIO_LOCK(sc);

	/* CTRL: FUNCSEL=GPIO, OUTOVER=PERI (0), OEOVER=PERI (0) */
	ctrl = rp1_io_read(sc, pin);
	ctrl &= ~(RP1_CTRL_FUNCSEL_MASK |
		  RP1_CTRL_OUTOVER_MASK  |
		  RP1_CTRL_OEOVER_MASK);
	ctrl |= RP1_FSEL_GPIO;
	rp1_io_write(sc, pin, ctrl);

	/* RIO: set or clear OE atomically */
	if (flags & GPIO_PIN_OUTPUT)
		rp1_rio_write(sc, bank, RP1_RIO_SET, RP1_RIO_OE, bit);
	else
		rp1_rio_write(sc, bank, RP1_RIO_CLR, RP1_RIO_OE, bit);

	/* PADS: ensure input-enable is set so IN_SYNC reflects the pad */
	pad = rp1_pad_read(sc, pin);
	pad |= RP1_PAD_IE;
	rp1_pad_write(sc, pin, pad);

	sc->sc_pins[pin].gp_flags = flags &
	    (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT | GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN);

	RP1_GPIO_UNLOCK(sc);
	return (0);
}

static int
rp1_gpio_pin_get(device_t dev, uint32_t pin, unsigned int *val)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	uint32_t in;
	int bank;

	if (pin >= sc->sc_npins)
		return (EINVAL);

	bank = RP1_GPIO_BANK(pin);
	RP1_GPIO_LOCK(sc);
	in = rp1_rio_read(sc, bank, RP1_RIO_RW, RP1_RIO_IN_SYNC);
	RP1_GPIO_UNLOCK(sc);

	*val = (in >> RP1_GPIO_PIN_IN_BANK(pin)) & 1u;
	return (0);
}

static int
rp1_gpio_pin_set(device_t dev, uint32_t pin, unsigned int val)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	uint32_t bit;
	int bank;

	if (pin >= sc->sc_npins)
		return (EINVAL);

	bank = RP1_GPIO_BANK(pin);
	bit  = 1u << RP1_GPIO_PIN_IN_BANK(pin);

	RP1_GPIO_LOCK(sc);
	rp1_rio_write(sc, bank,
	    val ? RP1_RIO_SET : RP1_RIO_CLR, RP1_RIO_OUT, bit);
	RP1_GPIO_UNLOCK(sc);

	return (0);
}

static int
rp1_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	uint32_t bit;
	int bank;

	if (pin >= sc->sc_npins)
		return (EINVAL);

	bank = RP1_GPIO_BANK(pin);
	bit  = 1u << RP1_GPIO_PIN_IN_BANK(pin);

	RP1_GPIO_LOCK(sc);
	rp1_rio_write(sc, bank, RP1_RIO_XOR, RP1_RIO_OUT, bit);
	RP1_GPIO_UNLOCK(sc);

	return (0);
}

/*
 * Decode a 2-cell GPIO specifier from a consumer DT node.
 * Cell 0 = pin number, cell 1 = GPIO_ACTIVE_LOW flag.
 */
static int
rp1_gpio_map_gpios(device_t bus, phandle_t dev __unused,
    phandle_t gparent __unused, int gcells __unused,
    pcell_t *gpios, uint32_t *pin, uint32_t *flags)
{
	*pin   = gpios[0];
	*flags = gpios[1];
	return (0);
}

/* -----------------------------------------------------------------------
 * fdt_pinctrl_if method — no-op in M1; M2 installs the function table.
 * fdt_pinctrl_register is skipped here because ofw_bus_get_node(dev)
 * returns -1 for this synthetic nexus device.  M2 will resolve the
 * FDT/device-framework integration.
 * ----------------------------------------------------------------------- */
static int
rp1_gpio_configure_pins(device_t dev __unused, phandle_t cfgxref __unused)
{
	return (0);
}

/* -----------------------------------------------------------------------
 * Device method table
 * ----------------------------------------------------------------------- */
static device_method_t rp1_gpio_methods[] = {
	/* Device interface */
	DEVMETHOD(device_identify,		rp1_gpio_identify),
	DEVMETHOD(device_probe,			rp1_gpio_probe),
	DEVMETHOD(device_attach,		rp1_gpio_attach),
	DEVMETHOD(device_detach,		rp1_gpio_detach),

	/* Bus interface (pass-through for gpiobus child) */
	DEVMETHOD(bus_alloc_resource,		bus_generic_alloc_resource),
	DEVMETHOD(bus_activate_resource,	bus_generic_activate_resource),
	DEVMETHOD(bus_deactivate_resource,	bus_generic_deactivate_resource),
	DEVMETHOD(bus_release_resource,		bus_generic_release_resource),
	DEVMETHOD(bus_setup_intr,		bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,		bus_generic_teardown_intr),

	/* gpio_if(9) */
	DEVMETHOD(gpio_get_bus,			rp1_gpio_get_bus),
	DEVMETHOD(gpio_pin_max,			rp1_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname,		rp1_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getcaps,		rp1_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_getflags,		rp1_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_setflags,		rp1_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,			rp1_gpio_pin_get),
	DEVMETHOD(gpio_pin_set,			rp1_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,		rp1_gpio_pin_toggle),
	DEVMETHOD(gpio_map_gpios,		rp1_gpio_map_gpios),

	/* fdt_pinctrl_if — configure_pins is a no-op in M1 */
	DEVMETHOD(fdt_pinctrl_configure,	rp1_gpio_configure_pins),

	DEVMETHOD_END
};

static driver_t rp1_gpio_driver = {
	"rp1_gpio",
	rp1_gpio_methods,
	sizeof(struct rp1_gpio_softc),
};

DRIVER_MODULE(rp1_gpio, nexus, rp1_gpio_driver, 0, 0);
/* Register gpiobus as a child driver so device_probe_and_attach succeeds. */
extern driver_t gpiobus_driver;
DRIVER_MODULE(gpiobus, rp1_gpio, gpiobus_driver, 0, 0);
MODULE_VERSION(rp1_gpio, 1);
MODULE_DEPEND(rp1_gpio, gpiobus, 1, 1, 1);
