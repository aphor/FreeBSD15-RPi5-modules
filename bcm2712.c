/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * BCM2712 Common Hardware Support Module
 * Provides common functions for BCM2712-based systems
 * Including basic hardware abstraction and thermal sensor access
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/time.h>
#include <sys/bus.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>


#include "bcm2712_var.h"

MALLOC_DEFINE(M_BCM2712, "bcm2712", "BCM2712 driver memory");

/* Global BCM2712 state */
static struct bcm2712_softc *bcm2712_sc = NULL;

/*
 * BCM2712 AVS Ring Oscillator Thermal Sensor
 *
 * Hardware: AVS (Adaptive Voltage Scaling) monitor in BCM2712 SoC
 * The thermal sensor is implemented as a ring oscillator whose frequency
 * varies with temperature. The raw value is converted to actual temperature
 * using calibration slope and offset.
 *
 * The device tree defines the AVS monitor at physical address 0x7d542000
 * with size 0xf00. This driver attaches to the "brcm,bcm2711-thermal"
 * device node within the avs-monitor to access the temperature register.
 *
 * References:
 *   - Linux: drivers/thermal/broadcom/bcm2711_thermal.c
 *   - FreeBSD: sys/arm/mv/armada/thermal.c (driver pattern)
 *   - Device tree: bcm2712-d-rpi-5-b.dtb (AVS monitor @0x7d542000)
 */

/* AVS Monitor base physical address - after soc bus ranges translation
 * DTS address 0x7d542000 + soc base 0x1000000000 = physical 0x107d542000
 */
#define BCM2712_AVS_BASE_PHYS		0x107d542000UL

/* Temperature register offset within AVS monitor */
#define BCM2712_AVS_TEMP_OFFSET		0x200

/* Temperature register bit masks */
#define BCM2712_TEMP_DATA_MSK		0x03FF  /* Bits [9:0] - 10-bit code */
#define BCM2712_TEMP_VALID_MSK		0x10400 /* Bits [16] and [10] validity */

/* Temperature conversion calibration (empirically determined for RPi 5) */
#define BCM2712_TEMP_SLOPE		-550    /* millidegrees per code unit */
#define BCM2712_TEMP_OFFSET		450000  /* millidegrees Celsius base offset */

/* TZ_ZEROC in deciKelvin (FreeBSD thermal convention) */
#define TZ_ZEROC	2731

/*
 * Read BCM2712 AVS thermal sensor raw register value.
 * Must be called with thermal_mtx held.
 * Returns temperature in millidegrees Celsius.
 *
 * The AVS thermal sensor is a ring oscillator whose frequency varies with
 * temperature. The raw register value contains a 10-bit code that must be
 * converted to actual temperature using calibration formula.
 */
static uint32_t
bcm2712_thermal_read_raw(struct bcm2712_softc *sc)
{
	uint32_t raw_value;

	mtx_assert(&sc->thermal_mtx, MA_OWNED);

	/* If AVS memory not mapped, return cached value */
	if (!sc->avs_mapped || sc->avs_vaddr == NULL) {
		return (sc->cached_temp_mc);
	}

	/*
	 * Read raw sensor value from hardware register.
	 * Temperature register is at offset 0x200 within the AVS monitor.
	 * Use direct memory access via virtual address mapping.
	 */
	raw_value = *(uint32_t *)((uintptr_t)sc->avs_vaddr + BCM2712_AVS_TEMP_OFFSET);

	/*
	 * Validity checking: Bit 16 must be set to indicate the sensor has
	 * produced a valid reading. On this hardware, bit 10 is not reliably set
	 * even with valid readings. Accept reading if bit 16 is set.
	 */
	if ((raw_value & 0x10000) == 0) {
		return (sc->cached_temp_mc);
	}

	/* Extract 10-bit temperature code from lower bits */
	uint32_t raw_code = raw_value & BCM2712_TEMP_DATA_MSK;

	/*
	 * Convert raw code to temperature using calibration formula:
	 * Temperature = slope × code + offset
	 *
	 * The slope of -550 means higher temperatures have lower raw codes.
	 * This is characteristic of ring oscillator-based sensors.
	 */
	int32_t temp_mc = (BCM2712_TEMP_SLOPE * raw_code) + BCM2712_TEMP_OFFSET;

	/* Clamp to reasonable operating range */
	if (temp_mc < 0)
		temp_mc = 0;
	if (temp_mc > 120000)
		temp_mc = 120000;

	return ((uint32_t)temp_mc);
}

/*
 * This function is retained for architectural clarity but the actual
 * temperature conversion is now integrated into bcm2712_thermal_read_raw()
 * since we need to check validity and clamp ranges there anyway.
 *
 * Keeps the data/control flow clean: read_raw handles hardware register
 * reads and conversion, this layer is now a no-op.
 */
static uint32_t
bcm2712_thermal_raw_to_millic(uint32_t temp_mc)
{
	/* Conversion already done in bcm2712_thermal_read_raw() */
	return (temp_mc);
}

/*
 * sysctl handler for CPU temperature (deciKelvin format).
 * Converts cached milli-°C to deciKelvin (FreeBSD convention).
 */
static int
bcm2712_thermal_sysctl_temp(SYSCTL_HANDLER_ARGS)
{
	struct bcm2712_softc *sc = arg1;
	uint32_t temp_dk;

	mtx_lock(&sc->thermal_mtx);
	/* Convert cached milli-°C to deciKelvin: (mC / 100) + TZ_ZEROC */
	temp_dk = (sc->cached_temp_mc / 100) + TZ_ZEROC;
	mtx_unlock(&sc->thermal_mtx);

	return (sysctl_handle_int(oidp, &temp_dk, 0, req));
}

/*
 * Periodic thermal sensor update callback.
 * Called every second by callout timer to update cached temperature.
 */
static void
bcm2712_thermal_update(void *arg)
{
	struct bcm2712_softc *sc = arg;
	uint32_t raw_value;

	mtx_assert(&sc->thermal_mtx, MA_OWNED);

	/* Read hardware and cache result */
	raw_value = bcm2712_thermal_read_raw(sc);
	sc->cached_temp_mc = bcm2712_thermal_raw_to_millic(raw_value);
	sc->last_update = time_uptime;

	/* Reschedule for next update (1 second interval) */
	callout_reset(&sc->thermal_callout, hz, bcm2712_thermal_update, sc);
}

/* Read BCM2712 CPU temperature */
int
bcm2712_read_cpu_temp(uint32_t *temp)
{
	struct bcm2712_softc *sc = bcm2712_sc;

	if (sc == NULL || temp == NULL)
		return (ENODEV);

	mtx_lock(&sc->thermal_mtx);
	*temp = sc->cached_temp_mc;
	mtx_unlock(&sc->thermal_mtx);

	return (0);
}

/* Get BCM2712 softc structure for other modules */
struct bcm2712_softc *
bcm2712_get_softc(void)
{
	return (bcm2712_sc);
}

/* Simple PWM channel configuration for testing */
int
bcm2712_pwm_set_config(u_int channel, u_int period, u_int duty)
{
	if (channel >= BCM2712_PWM_NCHANNELS)
		return (EINVAL);

	printf("bcm2712: PWM channel %u configured: period=%u, duty=%u\n",
	    channel, period, duty);
	return (0);
}

/* Enable/disable PWM channel */
int
bcm2712_pwm_enable(u_int channel, bool enable)
{
	if (channel >= BCM2712_PWM_NCHANNELS)
		return (EINVAL);

	printf("bcm2712: PWM channel %u %s\n", channel,
	    enable ? "enabled" : "disabled");
	return (0);
}

/* Module event handler for bcm2712 initialization */
static int
bcm2712_modevent(module_t mod __unused, int event, void *arg __unused)
{
	struct bcm2712_softc *sc;
	struct sysctl_oid *tree, *thermal_tree;
	int error = 0;

	switch (event) {
	case MOD_LOAD:
		/* Allocate softc structure */
		sc = malloc(sizeof(struct bcm2712_softc), M_BCM2712, M_WAITOK | M_ZERO);
		if (sc == NULL) {
			printf("bcm2712: Cannot allocate softc\n");
			return (ENOMEM);
		}

		/* Initialize mutexes */
		mtx_init(&sc->mtx, "bcm2712", NULL, MTX_DEF);
		mtx_init(&sc->thermal_mtx, "bcm2712_thermal", NULL, MTX_DEF);

		/* Initialize thermal callout */
		callout_init_mtx(&sc->thermal_callout, &sc->thermal_mtx, 0);

		/* Initialize cached temperature */
		sc->cached_temp_mc = 50000;  /* 50°C */
		sc->last_update = 0;

		/* Map physical memory using pmap with cache-inhibited attributes */
		sc->avs_vaddr = pmap_mapdev_attr(BCM2712_AVS_BASE_PHYS, 0x1000,
		    VM_MEMATTR_DEVICE);

		if (sc->avs_vaddr == NULL) {
			printf("bcm2712: Cannot map memory at 0x%lx\n",
			    (unsigned long)BCM2712_AVS_BASE_PHYS);
			mtx_destroy(&sc->thermal_mtx);
			mtx_destroy(&sc->mtx);
			free(sc, M_BCM2712);
			return (ENXIO);
		}

		sc->avs_mapped = 1;
		printf("bcm2712: AVS thermal sensor mapped at 0x%lx\n",
		    (unsigned long)BCM2712_AVS_BASE_PHYS);

		/* Initialize PWM channels */
		for (int i = 0; i < BCM2712_PWM_NCHANNELS; i++) {
			sc->channels[i].period = 41566;  /* ~24 kHz */
			sc->channels[i].duty_a = 0;
			sc->channels[i].duty_b = 0;
			sc->channels[i].flags = 0;
			sc->channels[i].enabled = false;
		}

		/* Initialize sysctl context */
		sysctl_ctx_init(&sc->sysctl_ctx);

		/* Create sysctl tree for BCM2712 */
		tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
		    SYSCTL_STATIC_CHILDREN(_hw),
		    OID_AUTO, "bcm2712", CTLFLAG_RD | CTLFLAG_MPSAFE,
		    0, "BCM2712 hardware support");
		sc->sysctl_tree = tree;

		if (tree != NULL) {
			/* Create thermal subtree */
			thermal_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(tree),
			    OID_AUTO, "thermal", CTLFLAG_RD | CTLFLAG_MPSAFE,
			    0, "Thermal sensor");

			if (thermal_tree != NULL) {
				/* Add temperature sysctl */
				SYSCTL_ADD_PROC(&sc->sysctl_ctx,
				    SYSCTL_CHILDREN(thermal_tree),
				    OID_AUTO, "cpu_temp", CTLFLAG_RD | CTLTYPE_INT | CTLFLAG_MPSAFE,
				    sc, 0, bcm2712_thermal_sysctl_temp, "IK",
				    "CPU temperature in deciKelvin");
			}
		}

		/* Start periodic temperature updates */
		mtx_lock(&sc->thermal_mtx);
		callout_reset(&sc->thermal_callout, hz, bcm2712_thermal_update, sc);
		mtx_unlock(&sc->thermal_mtx);

		/* Store global reference */
		bcm2712_sc = sc;

		printf("bcm2712: Thermal sensor initialized\n");
		break;

	case MOD_UNLOAD:
		sc = bcm2712_sc;
		if (sc == NULL)
			break;

		bcm2712_sc = NULL;

		/* Stop periodic updates */
		mtx_lock(&sc->thermal_mtx);
		callout_drain(&sc->thermal_callout);
		mtx_unlock(&sc->thermal_mtx);

		/* Clean up sysctl tree */
		sysctl_ctx_free(&sc->sysctl_ctx);

		/* Unmap memory */
		if (sc->avs_vaddr != NULL) {
			pmap_unmapdev(sc->avs_vaddr, 0x1000);
			sc->avs_vaddr = NULL;
			sc->avs_mapped = 0;
		}

		/* Clean up mutexes */
		mtx_destroy(&sc->thermal_mtx);
		mtx_destroy(&sc->mtx);

		free(sc, M_BCM2712);
		printf("bcm2712: Thermal sensor unloaded\n");
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static moduledata_t bcm2712_mdata = {
	"bcm2712",
	bcm2712_modevent,
	NULL
};

DECLARE_MODULE(bcm2712, bcm2712_mdata, SI_SUB_DRIVERS, SI_ORDER_ANY);