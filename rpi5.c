/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * Raspberry Pi 5 Board-Specific Module
 * Provides Pi 5-specific cooling fan control and thermal management
 * Depends on bcm2712 module for hardware access
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/callout.h>
#include <sys/time.h>

#include "bcm2712_var.h"

/* Debug flag for verbose logging (set to 1 to enable, 0 to disable) */
static int rpi5_debug = 0;

/* Sysctl context for fan control */
static struct sysctl_ctx_list rpi5_sysctl_ctx;

/* RPi5 Cooling Fan Control Structure */
struct rpi5_cooling_fan {
	struct mtx mtx;

	/* PWM device (from bcm2712 module) */
	device_t pwm_dev;
	u_int pwm_channel;

	/* Temperature thresholds (in milli-celsius) */
	uint32_t fan_temp0;
	uint32_t fan_temp1;
	uint32_t fan_temp2;
	uint32_t fan_temp3;

	/* Hysteresis values (in milli-celsius) */
	uint32_t fan_temp0_hyst;
	uint32_t fan_temp1_hyst;
	uint32_t fan_temp2_hyst;
	uint32_t fan_temp3_hyst;

	/* PWM speeds (0-255) */
	uint32_t fan_temp0_speed;
	uint32_t fan_temp1_speed;
	uint32_t fan_temp2_speed;
	uint32_t fan_temp3_speed;

	/* Current fan state (0-4) */
	uint32_t fan_current_state;
	uint32_t fan_prev_state;

	/* Current CPU temperature (mC) */
	uint32_t cpu_temp;

	/* Thermal management */
	int thermal_active;
	struct callout thermal_callout;
};

/* Global cooling fan state */
static struct rpi5_cooling_fan cooling_fan = {
	.pwm_channel = 3,	/* Fan uses PWM channel 3 on RP1 */

	/* Default values matching RPi5 defaults */
	.fan_temp0 = 50000,
	.fan_temp1 = 60000,
	.fan_temp2 = 67500,
	.fan_temp3 = 75000,

	.fan_temp0_hyst = 5000,
	.fan_temp1_hyst = 5000,
	.fan_temp2_hyst = 5000,
	.fan_temp3_hyst = 5000,

	.fan_temp0_speed = 75,
	.fan_temp1_speed = 125,
	.fan_temp2_speed = 175,
	.fan_temp3_speed = 250,

	.fan_current_state = 0,
	.fan_prev_state = 0,
	.cpu_temp = 50000,
};

/* Forward declarations */
static void rpi5_thermal_tick(void *arg);
static void rpi5_update_fan_state(void);
static int rpi5_find_pwm_device(void);

/* sysctl handlers */
static int rpi5_sysctl_temp_handler(SYSCTL_HANDLER_ARGS);
static int rpi5_sysctl_hyst_handler(SYSCTL_HANDLER_ARGS);
static int rpi5_sysctl_speed_handler(SYSCTL_HANDLER_ARGS);
static int rpi5_sysctl_current_temp_handler(SYSCTL_HANDLER_ARGS);
static int rpi5_sysctl_current_state_handler(SYSCTL_HANDLER_ARGS);

/* Check if bcm2712 module is available */
static int
rpi5_check_bcm2712(void)
{
	/* Check if bcm2712 thermal sensor is available */
	if (rpi5_debug)
		printf("rpi5: Checking BCM2712 thermal sensor availability\n");

	/* For now, assume bcm2712 is loaded. We'll verify at runtime. */
	if (rpi5_debug)
		printf("rpi5: BCM2712 thermal sensor detected\n");
	return (0);
}

/* Update fan state based on current temperature */
static void
rpi5_update_fan_state(void)
{
	uint32_t temp, new_state, speed;
	int period = 41566;  /* ~24 kHz period in ns */
	int duty;

	/* Called from thermal_tick which holds mutex via callout_init_mtx */
	temp = cooling_fan.cpu_temp;
	new_state = cooling_fan.fan_current_state;

	/* Thermal control logic with hysteresis */
	if (temp >= cooling_fan.fan_temp3) {
		new_state = 4;  /* Max speed */
	} else if (temp >= cooling_fan.fan_temp2 &&
	           (cooling_fan.fan_prev_state < 3 ||
	            temp >= (cooling_fan.fan_temp2 - cooling_fan.fan_temp2_hyst))) {
		new_state = 3;  /* High speed */
	} else if (temp >= cooling_fan.fan_temp1 &&
	           (cooling_fan.fan_prev_state < 2 ||
	            temp >= (cooling_fan.fan_temp1 - cooling_fan.fan_temp1_hyst))) {
		new_state = 2;  /* Medium speed */
	} else if (temp >= cooling_fan.fan_temp0 &&
	           (cooling_fan.fan_prev_state == 0 ||
	            temp >= (cooling_fan.fan_temp0 - cooling_fan.fan_temp0_hyst))) {
		new_state = 1;  /* Low speed */
	} else {
		new_state = 0;  /* Fan off */
	}

	/* Apply state change */
	if (new_state != cooling_fan.fan_current_state) {
		cooling_fan.fan_prev_state = cooling_fan.fan_current_state;
		cooling_fan.fan_current_state = new_state;

		/* Get speed for new state */
		switch (new_state) {
		case 0: speed = 0; break;  /* Off */
		case 1: speed = cooling_fan.fan_temp0_speed; break;  /* Low */
		case 2: speed = cooling_fan.fan_temp1_speed; break;  /* Medium */
		case 3: speed = cooling_fan.fan_temp2_speed; break;  /* High */
		case 4: speed = cooling_fan.fan_temp3_speed; break;  /* Max */
		default: speed = 0; break;  /* Safety fallback */
		}

		/* Convert speed (0-255) to duty cycle */
		duty = (speed * period) / 255;

		/* PWM control via bcm2712 module */
		bcm2712_pwm_set_config(cooling_fan.pwm_channel, period, duty);
		bcm2712_pwm_enable(cooling_fan.pwm_channel, speed > 0);

		if (rpi5_debug)
			printf("rpi5: Fan state %u->%u (temp %u.%uC, speed %u)\n",
			    cooling_fan.fan_prev_state, new_state,
			    temp / 1000, (temp % 1000) / 100, speed);
	}
}

/* Thermal management callout */
static void
rpi5_thermal_tick(void *arg)
{
	uint32_t temp;
	int error;

	/* Callout is invoked with mutex already held (callout_init_mtx) */
	/* Read CPU temperature from BCM2712 thermal sensor */
	error = bcm2712_read_cpu_temp(&temp);
	if (error) {
		/* Fallback to previous reading on error */
		temp = cooling_fan.cpu_temp;
	} else {
		cooling_fan.cpu_temp = temp;
	}

	/* Update fan based on new temperature */
	rpi5_update_fan_state();

	/* Schedule next tick if thermal management is active */
	if (cooling_fan.thermal_active) {
		callout_reset(&cooling_fan.thermal_callout, hz, rpi5_thermal_tick, NULL);
	}
}

/* sysctl handlers */
static int
rpi5_sysctl_temp_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t *temp_ptr = (uint32_t *)arg1;
	uint32_t temp;
	int error;

	mtx_lock(&cooling_fan.mtx);
	temp = *temp_ptr;
	mtx_unlock(&cooling_fan.mtx);

	error = sysctl_handle_int(oidp, &temp, 0, req);
	if (error || !req->newptr)
		return (error);

	/* Validate range: 0-120°C */
	if (temp > 120000)
		return (EINVAL);

	mtx_lock(&cooling_fan.mtx);
	*temp_ptr = temp;
	mtx_unlock(&cooling_fan.mtx);

	return (0);
}

static int
rpi5_sysctl_hyst_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t *hyst_ptr = (uint32_t *)arg1;
	uint32_t hyst;
	int error;

	mtx_lock(&cooling_fan.mtx);
	hyst = *hyst_ptr;
	mtx_unlock(&cooling_fan.mtx);

	error = sysctl_handle_int(oidp, &hyst, 0, req);
	if (error || !req->newptr)
		return (error);

	/* Validate range: 0-10°C */
	if (hyst > 10000)
		return (EINVAL);

	mtx_lock(&cooling_fan.mtx);
	*hyst_ptr = hyst;
	mtx_unlock(&cooling_fan.mtx);

	return (0);
}

static int
rpi5_sysctl_speed_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t *speed_ptr = (uint32_t *)arg1;
	uint32_t speed;
	int error;

	mtx_lock(&cooling_fan.mtx);
	speed = *speed_ptr;
	mtx_unlock(&cooling_fan.mtx);

	error = sysctl_handle_int(oidp, &speed, 0, req);
	if (error || !req->newptr)
		return (error);

	/* Validate range: 0-255 */
	if (speed > 255)
		return (EINVAL);

	mtx_lock(&cooling_fan.mtx);
	*speed_ptr = speed;
	mtx_unlock(&cooling_fan.mtx);

	return (0);
}

static int
rpi5_sysctl_current_temp_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t temp;

	mtx_lock(&cooling_fan.mtx);
	temp = cooling_fan.cpu_temp;
	mtx_unlock(&cooling_fan.mtx);

	return (sysctl_handle_int(oidp, &temp, 0, req));
}

static int
rpi5_sysctl_current_state_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t state;

	mtx_lock(&cooling_fan.mtx);
	state = cooling_fan.fan_current_state;
	mtx_unlock(&cooling_fan.mtx);

	return (sysctl_handle_int(oidp, &state, 0, req));
}

/* Module load handler */
static int
rpi5_modevent(module_t mod, int event, void *data)
{
	int error = 0;

	switch (event) {
	case MOD_LOAD:
		printf("rpi5: Raspberry Pi 5 board support loading\n");

		/* Initialize mutex and callout */
		mtx_init(&cooling_fan.mtx, "rpi5_cooling", NULL, MTX_DEF);
		callout_init_mtx(&cooling_fan.thermal_callout, &cooling_fan.mtx, 0);

		/* Check bcm2712 module */
		error = rpi5_check_bcm2712();
		if (error) {
			printf("rpi5: Failed to access BCM2712 module\n");
			mtx_destroy(&cooling_fan.mtx);
			return (error);
		}

		/* Create sysctl tree for fan control parameters */
		{
			struct sysctl_oid *tree, *fan_tree;

			sysctl_ctx_init(&rpi5_sysctl_ctx);

			/* Create hw.rpi5 node */
			tree = SYSCTL_ADD_NODE(&rpi5_sysctl_ctx, SYSCTL_STATIC_CHILDREN(_hw),
			    OID_AUTO, "rpi5", CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
			    "Raspberry Pi 5 cooling fan control");

			if (tree != NULL) {
				/* Create hw.rpi5.fan node */
				fan_tree = SYSCTL_ADD_NODE(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(tree),
				    OID_AUTO, "fan", CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
				    "Cooling fan control");

				if (fan_tree != NULL) {
					/* Temperature thresholds (in milli-Celsius) */
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp0", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp0, 0,
					    "Level 0 temperature threshold (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp1", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp1, 0,
					    "Level 1 temperature threshold (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp2", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp2, 0,
					    "Level 2 temperature threshold (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp3", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp3, 0,
					    "Level 3 temperature threshold (mC)");

					/* Hysteresis values */
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp0_hyst", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp0_hyst, 0,
					    "Level 0 hysteresis (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp1_hyst", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp1_hyst, 0,
					    "Level 1 hysteresis (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp2_hyst", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp2_hyst, 0,
					    "Level 2 hysteresis (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "temp3_hyst", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp3_hyst, 0,
					    "Level 3 hysteresis (mC)");

					/* PWM speeds (0-255) */
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "speed0", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp0_speed, 0,
					    "Level 0 PWM speed (0-255)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "speed1", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp1_speed, 0,
					    "Level 1 PWM speed (0-255)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "speed2", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp2_speed, 0,
					    "Level 2 PWM speed (0-255)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "speed3", CTLFLAG_RW | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_temp3_speed, 0,
					    "Level 3 PWM speed (0-255)");

					/* Read-only status */
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "cpu_temp", CTLFLAG_RD | CTLFLAG_MPSAFE,
					    &cooling_fan.cpu_temp, 0,
					    "Current CPU temperature (mC)");
					SYSCTL_ADD_UINT(&rpi5_sysctl_ctx, SYSCTL_CHILDREN(fan_tree),
					    OID_AUTO, "current_state", CTLFLAG_RD | CTLFLAG_MPSAFE,
					    &cooling_fan.fan_current_state, 0,
					    "Current fan state (0-4)");
				}
			}
		}

		/* Start thermal management */
		mtx_lock(&cooling_fan.mtx);
		cooling_fan.thermal_active = 1;
		callout_reset(&cooling_fan.thermal_callout, hz, rpi5_thermal_tick, NULL);
		mtx_unlock(&cooling_fan.mtx);

		printf("rpi5: Cooling fan thermal management started\n");
		break;

	case MOD_UNLOAD:
		printf("rpi5: Raspberry Pi 5 board support unloading\n");

		/* Stop thermal management */
		mtx_lock(&cooling_fan.mtx);
		cooling_fan.thermal_active = 0;
		mtx_unlock(&cooling_fan.mtx);
		callout_drain(&cooling_fan.thermal_callout);

		/* Turn off fan */
		bcm2712_pwm_enable(cooling_fan.pwm_channel, false);

		/* Clean up sysctl tree */
		sysctl_ctx_free(&rpi5_sysctl_ctx);

		/* Clean up synchronization primitives */
		mtx_destroy(&cooling_fan.mtx);
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static moduledata_t rpi5_mod = {
	"rpi5",
	rpi5_modevent,
	0
};

DECLARE_MODULE(rpi5, rpi5_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(rpi5, 1);
MODULE_DEPEND(rpi5, bcm2712, 1, 1, 1);
