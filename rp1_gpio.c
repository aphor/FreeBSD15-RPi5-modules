/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * rp1_gpio — RP1 GPIO / Pinctrl Controller
 *
 * Milestone 1: probe/attach skeleton, 3-window MMIO mapping, full gpio_if(9)
 * implementation (pin_get/set/toggle/flags/caps/name), fdt_pinctrl stub,
 * and IRQ-parent chain validation via bus_alloc_resource for each bank IRQ.
 *
 * Milestone 2: rp1_gpio_func.c function table + fdt_pinctrl configure_pins.
 * Milestone 3: PADS pull / drive-strength / schmitt in pin_setflags.
 * Milestone 4: pic_if per-pin edge/level interrupts.
 *
 * Pattern: sys/arm/broadcom/bcm2835/bcm2835_gpio.c
 * Hardware: RP-008370-DS-1 §3 (IO_BANK, SYS_RIO, PADS_BANK)
 * FDT node: gpio@d0000, compatible "raspberrypi,rp1-gpio"
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>

#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>

#include "gpio_if.h"
#include "fdt_pinctrl_if.h"
#include "rp1_gpio_var.h"

/* -----------------------------------------------------------------------
 * Memory resource spec — 3 windows from the DTB reg property, in order:
 * RID 0 = IO_BANK (CTRL/STATUS), RID 1 = SYS_RIO (OUT/OE/IN), RID 2 = PADS.
 * IRQs are allocated separately (optional; see rp1_gpio_attach).
 * ----------------------------------------------------------------------- */
static struct resource_spec rp1_gpio_mem_spec[] = {
	{ SYS_RES_MEMORY, 0, RF_ACTIVE },	/* IO_BANK0..2 */
	{ SYS_RES_MEMORY, 1, RF_ACTIVE },	/* SYS_RIO0..2 */
	{ SYS_RES_MEMORY, 2, RF_ACTIVE },	/* PADS_BANK0..2 */
	{ -1, 0 }
};

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
 * Probe / attach / detach
 * ----------------------------------------------------------------------- */
static int rp1_gpio_detach(device_t dev);	/* forward declaration */

static int
rp1_gpio_probe(device_t dev)
{
	if (!ofw_bus_is_compatible(dev, "raspberrypi,rp1-gpio"))
		return (ENXIO);
	device_set_desc(dev, "RP1 GPIO / Pinctrl Controller");
	return (BUS_PROBE_DEFAULT);
}

static int
rp1_gpio_attach(device_t dev)
{
	struct rp1_gpio_softc *sc;
	phandle_t node;
	uint32_t ctrl, funcsel, oe;
	int bank, i, rid;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_npins = RP1_NUM_GPIOS;

	if (bus_alloc_resources(dev, rp1_gpio_mem_spec, sc->sc_res) != 0) {
		device_printf(dev, "cannot allocate memory resources\n");
		return (ENXIO);
	}

	mtx_init(&sc->sc_mtx, "rp1gpio", NULL, MTX_SPIN);

	/*
	 * M1 task 4: attempt IRQ allocation for each bank to validate the
	 * FDT interrupt-parent chain through the RP1 simplebus and PCIe bridge.
	 * Failures here are non-fatal; M4 will address bridging if needed.
	 */
	for (i = 0; i < 3; i++) {
		rid = i;
		sc->sc_res[RES_IRQ0 + i] = bus_alloc_resource_any(dev,
		    SYS_RES_IRQ, &rid, RF_ACTIVE | RF_SHAREABLE);
		if (sc->sc_res[RES_IRQ0 + i] != NULL)
			device_printf(dev, "bank%d IRQ: chain OK\n", i);
		else
			device_printf(dev,
			    "bank%d IRQ: not deliverable (M4 may need bridging shim)\n",
			    i);
	}

	/* Populate pin table: names from gpio-line-names, flags from FUNCSEL/OE */
	node = ofw_bus_get_node(dev);
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
	 * Register as fdt_pinctrl provider using the "function" property name
	 * (Linux RP1 binding).  configure_pins is a no-op in M1; M2 fills it.
	 */
	fdt_pinctrl_register(dev, "function");
	fdt_pinctrl_configure_tree(dev);

	sc->sc_busdev = gpiobus_add_bus(dev);
	if (sc->sc_busdev == NULL) {
		device_printf(dev, "cannot attach gpiobus\n");
		rp1_gpio_detach(dev);
		return (ENXIO);
	}

	return (0);
}

static int
rp1_gpio_detach(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	int i;

	if (sc->sc_busdev != NULL) {
		bus_generic_detach(dev);
		sc->sc_busdev = NULL;
	}

	for (i = 0; i < 3; i++) {
		if (sc->sc_res[RES_IRQ0 + i] != NULL) {
			bus_release_resource(dev, SYS_RES_IRQ, i,
			    sc->sc_res[RES_IRQ0 + i]);
			sc->sc_res[RES_IRQ0 + i] = NULL;
		}
	}

	bus_release_resources(dev, rp1_gpio_mem_spec, sc->sc_res);

	if (mtx_initialized(&sc->sc_mtx))
		mtx_destroy(&sc->sc_mtx);

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
 * IE is set in PADS so input reads reflect the pad voltage in both directions.
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
 * fdt_pinctrl_if method — no-op in M1; M2 installs the function table
 * ----------------------------------------------------------------------- */
static int
rp1_gpio_configure_pins(device_t dev, phandle_t cfgxref __unused)
{
	return (0);
}

/* -----------------------------------------------------------------------
 * Device method table
 * ----------------------------------------------------------------------- */
static device_method_t rp1_gpio_methods[] = {
	/* Device interface */
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

	/* OFW bus (exposes our DTB node to children / fdt_pinctrl) */
	DEVMETHOD(ofw_bus_get_node,		ofw_bus_gen_get_node),

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
	"gpio",
	rp1_gpio_methods,
	sizeof(struct rp1_gpio_softc),
};

DRIVER_MODULE(rp1_gpio, simplebus, rp1_gpio_driver, 0, 0);
MODULE_VERSION(rp1_gpio, 1);
MODULE_DEPEND(rp1_gpio, gpiobus, 1, 1, 1);
