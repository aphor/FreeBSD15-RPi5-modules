/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * Raspberry Pi 5 RP1 PWM Controller Driver
 * Provides pwmbus interface to RP1 PWM hardware
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pwm/pwmbus.h>
#include <dev/fdt/fdt_common.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

/* RP1 Register Definitions */
#define RP1_PWM_BASE			0x000e0000
#define RP1_PWM_OFFSET_CH(ch)		(0x2800 * (ch))

#define RP1_PWM_CSR(ch)			(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x00)
#define RP1_PWM_DIV(ch)			(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x04)
#define RP1_PWM_TOP(ch)			(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x08)
#define RP1_PWM_CC(ch)			(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x0c)
#define RP1_PWM_CTRA(ch)		(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x10)
#define RP1_PWM_CTRB(ch)		(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x14)
#define RP1_PWM_FRAC(ch)		(RP1_PWM_BASE + RP1_PWM_OFFSET_CH(ch) + 0x18)

/* CSR bit fields */
#define RP1_PWM_CSR_EN			(1 << 0)
#define RP1_PWM_CSR_NOINVERT_A		(1 << 1)
#define RP1_PWM_CSR_NOINVERT_B		(1 << 2)
#define RP1_PWM_CSR_DIVMODE(m)		((m) << 4)
#define RP1_PWM_CSR_PH_CORRECT		(1 << 9)
#define RP1_PWM_CSR_ACINVERT		(1 << 11)
#define RP1_PWM_CSR_BCKINVERT		(1 << 12)

/* RP1 PWM channel structure */
struct rp1_pwm_channel {
	uint32_t period;	/* Period in nanoseconds */
	uint32_t duty_a;	/* Duty cycle A (nanoseconds) */
	uint32_t duty_b;	/* Duty cycle B (nanoseconds) */
	uint32_t flags;		/* PWM flags (polarity, etc) */
	bool enabled;		/* Channel enabled */
};

/* RP1 PWM controller structure */
struct rp1_pwm_softc {
	device_t dev;
	struct mtx mtx;
	
	/* Memory resources */
	struct resource *mem;
	int mem_rid;
	bus_space_tag_t bst;
	bus_space_handle_t bsh;
	
	/* PWM channels (4 available on RP1) */
	struct rp1_pwm_channel channels[4];
	
	/* Thermal fan control state */
	struct {
		struct mtx mtx;
		uint32_t cpu_temp;	/* Current CPU temp in mC */
		uint32_t fan_state;	/* Current fan level (0-3) */
		uint32_t fan_prev_state; /* Previous state (hysteresis) */
	} thermal;
};

static int rp1_pwm_probe(device_t dev);
static int rp1_pwm_attach(device_t dev);
static int rp1_pwm_detach(device_t dev);

/* PWM bus interface */
static int rp1_pwm_channel_count(device_t dev, u_int *nchannels);
static int rp1_pwm_channel_config(device_t dev, u_int channel,
    u_int period, u_int duty);
static int rp1_pwm_channel_get_config(device_t dev, u_int channel,
    u_int *period, u_int *duty);
static int rp1_pwm_channel_set_flags(device_t dev, u_int channel,
    uint32_t flags);
static int rp1_pwm_channel_get_flags(device_t dev, u_int channel,
    uint32_t *flags);
static int rp1_pwm_channel_enable(device_t dev, u_int channel, bool enable);
static int rp1_pwm_channel_is_enabled(device_t dev, u_int channel, bool *enable);

/* Hardware register access functions */
static uint32_t
rp1_pwm_read(struct rp1_pwm_softc *sc, uint32_t offset)
{
	return (bus_space_read_4(sc->bst, sc->bsh, offset));
}

static void
rp1_pwm_write(struct rp1_pwm_softc *sc, uint32_t offset, uint32_t value)
{
	bus_space_write_4(sc->bst, sc->bsh, offset, value);
}

/* Configure PWM hardware for a channel */
static int
rp1_pwm_hw_config(struct rp1_pwm_softc *sc, u_int channel, u_int period,
    u_int duty)
{
	uint32_t csr, div, top, cc;
	uint32_t clk_div, clk_freq;
	
	if (channel >= 4)
		return (EINVAL);
	if (period == 0 || duty > period)
		return (EINVAL);
	
	mtx_lock(&sc->mtx);
	
	/* Calculate clock divider and TOP value
	 * RP1 PWM clock is 125 MHz
	 * period = (1 / (125MHz / div)) * (TOP + 1) ns
	 * We want to find div and TOP such that the period matches
	 */
	clk_freq = 125000000; /* 125 MHz */
	
	/* Try to find suitable div and top values */
	for (div = 1; div <= 256; div++) {
		top = (clk_freq / div) * period / 1000000000ULL;
		if (top > 0 && top <= 0xFFFF)
			break;
	}
	
	if (top > 0xFFFF || top == 0) {
		mtx_unlock(&sc->mtx);
		return (EINVAL);
	}
	
	/* Calculate compare count for duty cycle */
	cc = (duty * (top + 1)) / period;
	if (cc > top)
		cc = top;
	
	/* Read current CSR */
	csr = rp1_pwm_read(sc, RP1_PWM_CSR(channel));
	
	/* Disable channel during configuration */
	csr &= ~RP1_PWM_CSR_EN;
	rp1_pwm_write(sc, RP1_PWM_CSR(channel), csr);
	
	/* Set period (TOP register) */
	rp1_pwm_write(sc, RP1_PWM_TOP(channel), top);
	
	/* Set duty cycle */
	rp1_pwm_write(sc, RP1_PWM_CTRA(channel), cc);
	rp1_pwm_write(sc, RP1_PWM_CTRB(channel), cc);
	
	/* Set clock divider */
	rp1_pwm_write(sc, RP1_PWM_DIV(channel), div);
	
	/* Re-enable if it was enabled */
	if (sc->channels[channel].enabled)
		csr |= RP1_PWM_CSR_EN;
	
	rp1_pwm_write(sc, RP1_PWM_CSR(channel), csr);
	
	/* Store configuration */
	sc->channels[channel].period = period;
	sc->channels[channel].duty_a = cc;
	
	mtx_unlock(&sc->mtx);
	return (0);
}

/* PWM bus interface implementations */
static int
rp1_pwm_channel_count(device_t dev, u_int *nchannels)
{
	*nchannels = 4;
	return (0);
}

static int
rp1_pwm_channel_config(device_t dev, u_int channel, u_int period, u_int duty)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	
	return (rp1_pwm_hw_config(sc, channel, period, duty));
}

static int
rp1_pwm_channel_get_config(device_t dev, u_int channel, u_int *period,
    u_int *duty)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	
	if (channel >= 4)
		return (EINVAL);
	
	mtx_lock(&sc->mtx);
	*period = sc->channels[channel].period;
	*duty = sc->channels[channel].duty_a;
	mtx_unlock(&sc->mtx);
	
	return (0);
}

static int
rp1_pwm_channel_enable(device_t dev, u_int channel, bool enable)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	uint32_t csr;
	
	if (channel >= 4)
		return (EINVAL);
	
	mtx_lock(&sc->mtx);
	
	csr = rp1_pwm_read(sc, RP1_PWM_CSR(channel));
	if (enable)
		csr |= RP1_PWM_CSR_EN;
	else
		csr &= ~RP1_PWM_CSR_EN;
	
	rp1_pwm_write(sc, RP1_PWM_CSR(channel), csr);
	sc->channels[channel].enabled = enable;
	
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
rp1_pwm_channel_is_enabled(device_t dev, u_int channel, bool *enable)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	
	if (channel >= 4)
		return (EINVAL);
	
	mtx_lock(&sc->mtx);
	*enable = sc->channels[channel].enabled;
	mtx_unlock(&sc->mtx);
	
	return (0);
}

static int
rp1_pwm_channel_set_flags(device_t dev, u_int channel, uint32_t flags)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	uint32_t csr;
	
	if (channel >= 4)
		return (EINVAL);
	
	mtx_lock(&sc->mtx);
	
	csr = rp1_pwm_read(sc, RP1_PWM_CSR(channel));
	
	/* Handle polarity inversion */
	if (flags & PWM_POLARITY_INVERTED) {
		if (channel == 0 || channel == 1)
			csr &= ~RP1_PWM_CSR_NOINVERT_A;
		else
			csr &= ~RP1_PWM_CSR_NOINVERT_B;
	} else {
		if (channel == 0 || channel == 1)
			csr |= RP1_PWM_CSR_NOINVERT_A;
		else
			csr |= RP1_PWM_CSR_NOINVERT_B;
	}
	
	rp1_pwm_write(sc, RP1_PWM_CSR(channel), csr);
	sc->channels[channel].flags = flags;
	
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
rp1_pwm_channel_get_flags(device_t dev, u_int channel, uint32_t *flags)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	
	if (channel >= 4)
		return (EINVAL);
	
	mtx_lock(&sc->mtx);
	*flags = sc->channels[channel].flags;
	mtx_unlock(&sc->mtx);
	
	return (0);
}

/* Driver methods */
static int
rp1_pwm_probe(device_t dev)
{
	if (ofw_bus_is_compatible(dev, "raspberrypi,rp1-pwm")) {
		device_set_desc(dev, "Raspberry Pi 5 RP1 PWM Controller");
		return (BUS_PROBE_DEFAULT);
	}
	return (ENXIO);
}

static int
rp1_pwm_attach(device_t dev)
{
	struct rp1_pwm_softc *sc;
	int error;
	
	sc = device_get_softc(dev);
	sc->dev = dev;
	
	mtx_init(&sc->mtx, "rp1_pwm", NULL, MTX_DEF);
	mtx_init(&sc->thermal.mtx, "rp1_thermal", NULL, MTX_DEF);
	
	/* Allocate memory resource */
	sc->mem_rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->mem_rid,
	    RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "failed to allocate memory resource\n");
		mtx_destroy(&sc->mtx);
		mtx_destroy(&sc->thermal.mtx);
		return (ENXIO);
	}
	
	sc->bst = rman_get_bustag(sc->mem);
	sc->bsh = rman_get_bushandle(sc->mem);
	
	/* Initialize channel state */
	for (int i = 0; i < 4; i++) {
		sc->channels[i].period = 41566;	/* Default RPi5 fan period */
		sc->channels[i].duty_a = 0;
		sc->channels[i].flags = 0;
		sc->channels[i].enabled = false;
	}
	
	/* Initialize thermal state */
	sc->thermal.cpu_temp = 50000;
	sc->thermal.fan_state = 0;
	sc->thermal.fan_prev_state = 0;
	
	/* Register with pwmbus */
	error = pwmbus_register_provider(dev);
	if (error != 0) {
		device_printf(dev, "failed to register pwmbus provider\n");
		bus_release_resource(dev, SYS_RES_MEMORY, sc->mem_rid, sc->mem);
		mtx_destroy(&sc->mtx);
		mtx_destroy(&sc->thermal.mtx);
		return (error);
	}
	
	device_printf(dev, "RP1 PWM controller initialized with 4 channels\n");
	return (0);
}

static int
rp1_pwm_detach(device_t dev)
{
	struct rp1_pwm_softc *sc = device_get_softc(dev);
	
	pwmbus_unregister_provider(dev);
	
	if (sc->mem != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->mem_rid, sc->mem);
	
	mtx_destroy(&sc->mtx);
	mtx_destroy(&sc->thermal.mtx);
	
	return (0);
}

static device_method_t rp1_pwm_methods[] = {
	DEVMETHOD(device_probe,		rp1_pwm_probe),
	DEVMETHOD(device_attach,	rp1_pwm_attach),
	DEVMETHOD(device_detach,	rp1_pwm_detach),
	
	DEVMETHOD(pwmbus_channel_count,		rp1_pwm_channel_count),
	DEVMETHOD(pwmbus_channel_config,	rp1_pwm_channel_config),
	DEVMETHOD(pwmbus_channel_get_config,	rp1_pwm_channel_get_config),
	DEVMETHOD(pwmbus_channel_enable,	rp1_pwm_channel_enable),
	DEVMETHOD(pwmbus_channel_is_enabled,	rp1_pwm_channel_is_enabled),
	DEVMETHOD(pwmbus_channel_set_flags,	rp1_pwm_channel_set_flags),
	DEVMETHOD(pwmbus_channel_get_flags,	rp1_pwm_channel_get_flags),
	
	DEVMETHOD_END
};

static driver_t rp1_pwm_driver = {
	"rp1_pwm",
	rp1_pwm_methods,
	sizeof(struct rp1_pwm_softc)
};

DRIVER_MODULE(rp1_pwm, simplebus, rp1_pwm_driver, 0, 0);
MODULE_DEPEND(rp1_pwm, pwmbus, 1, 1, 1);
MODULE_VERSION(rp1_pwm, 1);
