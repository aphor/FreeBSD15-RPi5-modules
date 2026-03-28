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

#include "bcm2712_var.h"

MALLOC_DEFINE(M_BCM2712, "bcm2712", "BCM2712 driver memory");

/* Global BCM2712 state */
static struct bcm2712_softc *bcm2712_sc = NULL;

/* Read BCM2712 CPU temperature */
int
bcm2712_read_cpu_temp(uint32_t *temp)
{
	/* TODO: Implement actual BCM2712 thermal sensor reading */
	/* For now, return a placeholder value */
	*temp = 50000;  /* 50°C in milli-celsius */
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

/* Module load handler */
static int
bcm2712_modevent(module_t mod, int event, void *data)
{
	struct bcm2712_softc *sc;
	int error = 0;

	switch (event) {
	case MOD_LOAD:
		printf("bcm2712: BCM2712 common hardware support loading\n");

		/* Allocate softc structure */
		sc = malloc(sizeof(struct bcm2712_softc), M_BCM2712, M_WAITOK | M_ZERO);
		if (sc == NULL) {
			printf("bcm2712: Failed to allocate softc\n");
			return (ENOMEM);
		}

		/* Initialize mutex */
		mtx_init(&sc->mtx, "bcm2712", NULL, MTX_DEF);

		/* Initialize PWM channels */
		for (int i = 0; i < BCM2712_PWM_NCHANNELS; i++) {
			sc->channels[i].period = 41566;  /* ~24 kHz */
			sc->channels[i].duty_a = 0;
			sc->channels[i].duty_b = 0;
			sc->channels[i].flags = 0;
			sc->channels[i].enabled = false;
		}

		/* Store global reference */
		bcm2712_sc = sc;

		printf("bcm2712: BCM2712 common hardware support loaded\n");
		break;

	case MOD_UNLOAD:
		printf("bcm2712: BCM2712 common hardware support unloading\n");

		sc = bcm2712_sc;
		if (sc != NULL) {
			/* Clear global reference */
			bcm2712_sc = NULL;

			/* Clean up mutex */
			mtx_destroy(&sc->mtx);

			/* Free softc */
			free(sc, M_BCM2712);
		}

		printf("bcm2712: BCM2712 common hardware support unloaded\n");
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static moduledata_t bcm2712_mod = {
	"bcm2712",
	bcm2712_modevent,
	0
};

DECLARE_MODULE(bcm2712, bcm2712_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(bcm2712, 1);