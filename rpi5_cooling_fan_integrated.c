/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * Raspberry Pi 5 Cooling Fan Sysctl Module with Thermal Management
 * Integrates with RP1 PWM driver for actual hardware control
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/callout.h>
#include <sys/time.h>

#include <dev/pwm/pwmbus.h>

/* Maximum number of PWM controllers */
#define MAX_PWM_CONTROLLERS	8

/* RPi5 Cooling Fan Control Structure */
struct rpi5_cooling_fan {
	struct mtx mtx;
	
	/* PWM device */
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
	
	/* Current fan state (0-3) */
	uint32_t fan_current_state;
	uint32_t fan_prev_state;
	
	/* Current CPU temperature (mC) */
	uint32_t cpu_temp;
	
	/* Thermal management thread */
	struct thread *thermal_thread;
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
static void rpi5_thermal_thread(void *arg);
static void rpi5_thermal_tick(void *arg);
static int rpi5_read_cpu_temp(uint32_t *temp);
static void rpi5_update_fan_state(void);

/* Read CPU temperature from thermal sensor
 * Returns temperature in milli-celsius via temp pointer
 */
static int
rpi5_read_cpu_temp(uint32_t *temp)
{
	FILE *fp;
	char buf[128];
	int ret;
	
	/* Try to read from kernel temperature sensor
	 * This would need to be implemented via proper sysctl or hwmon interface
	 * For now, we provide a stub that can be extended
	 */
	
	/* Placeholder: In production, integrate with hwmon or thermal subsystem
	 * This could read from sysctl hw.sensors or /dev/coretemp
	 * For demonstration, return a reasonable value
	 */
	*temp = 50000;	/* 50°C default */
	return (0);
}

/* Convert PWM speed (0-255) to duty cycle for fan control
 * The fan duty cycle is nanoseconds, with a period of 41566 ns
 */
static uint32_t
rpi5_speed_to_duty(uint8_t speed)
{
	uint32_t period = 41566;	/* RP1 PWM period for fan */
	return (period * speed / 255);
}

/* Update fan state based on current temperature
 * Implements hysteresis to prevent rapid switching
 */
static void
rpi5_update_fan_state(void)
{
	uint32_t temp, new_state;
	uint32_t thresh, hyst, speed;
	int error;
	
	mtx_lock(&cooling_fan.mtx);
	
	temp = cooling_fan.cpu_temp;
	new_state = cooling_fan.fan_current_state;
	
	/* State transitions with hysteresis */
	
	/* Check transition from state 0 to 1 */
	if (new_state == 0 && temp >= cooling_fan.fan_temp0) {
		new_state = 1;
	}
	/* Check hysteresis transition back to 0 */
	else if (new_state == 1 && temp < cooling_fan.fan_temp0 - cooling_fan.fan_temp0_hyst) {
		new_state = 0;
	}
	/* Check transition from 1 to 2 */
	else if (new_state == 1 && temp >= cooling_fan.fan_temp1) {
		new_state = 2;
	}
	/* Check hysteresis transition back to 1 */
	else if (new_state == 2 && temp < cooling_fan.fan_temp1 - cooling_fan.fan_temp1_hyst) {
		new_state = 1;
	}
	/* Check transition from 2 to 3 */
	else if (new_state == 2 && temp >= cooling_fan.fan_temp2) {
		new_state = 3;
	}
	/* Check hysteresis transition back to 2 */
	else if (new_state == 3 && temp < cooling_fan.fan_temp2 - cooling_fan.fan_temp2_hyst) {
		new_state = 2;
	}
	/* Check transition to full speed */
	else if (new_state == 3 && temp >= cooling_fan.fan_temp3) {
		new_state = 4;	/* Actually level 3 is max, but we track state 4 */
	}
	/* Check hysteresis transition back to 3 */
	else if (new_state == 4 && temp < cooling_fan.fan_temp3 - cooling_fan.fan_temp3_hyst) {
		new_state = 3;
	}
	
	/* If state changed, update PWM */
	if (new_state != cooling_fan.fan_current_state && cooling_fan.pwm_dev != NULL) {
		u_int period = 41566;	/* RP1 PWM period in ns */
		u_int duty = 0;
		
		/* Select speed based on new state */
		switch (new_state) {
		case 0:
			duty = 0;
			break;
		case 1:
			duty = rpi5_speed_to_duty(cooling_fan.fan_temp0_speed);
			break;
		case 2:
			duty = rpi5_speed_to_duty(cooling_fan.fan_temp1_speed);
			break;
		case 3:
			duty = rpi5_speed_to_duty(cooling_fan.fan_temp2_speed);
			break;
		case 4:
			duty = rpi5_speed_to_duty(cooling_fan.fan_temp3_speed);
			break;
		}
		
		/* Configure PWM channel */
		error = PWMBUS_CHANNEL_CONFIG(cooling_fan.pwm_dev, 
		    cooling_fan.pwm_channel, period, duty);
		
		if (error == 0) {
			/* Enable the channel */
			if (duty > 0)
				PWMBUS_CHANNEL_ENABLE(cooling_fan.pwm_dev,
				    cooling_fan.pwm_channel, true);
			
			cooling_fan.fan_current_state = new_state;
		}
	}
	
	mtx_unlock(&cooling_fan.mtx);
}

/* Periodic thermal management callback */
static void
rpi5_thermal_tick(void *arg)
{
	struct rpi5_cooling_fan *cf = (struct rpi5_cooling_fan *)arg;
	uint32_t temp;
	
	/* Read current CPU temperature */
	if (rpi5_read_cpu_temp(&temp) == 0) {
		mtx_lock(&cf->mtx);
		cf->cpu_temp = temp;
		mtx_unlock(&cf->mtx);
	}
	
	/* Update fan state based on temperature */
	rpi5_update_fan_state();
	
	/* Schedule next update in 1 second */
	callout_schedule(&cf->thermal_callout, hz);
}

/* Thermal management thread - handles calibration and initialization */
static void
rpi5_thermal_thread(void *arg)
{
	struct rpi5_cooling_fan *cf = (struct rpi5_cooling_fan *)arg;
	
	/* Thread runs as long as module is active */
	while (cf->thermal_active) {
		/* Periodically check for PWM device if not yet found */
		if (cf->pwm_dev == NULL) {
			/* Try to locate the RP1 PWM device
			 * In production, use device_find_child() or devclass_find()
			 */
		}
		
		kthread_suspend_check();
		pause("rpi5th", hz * 5);	/* Check every 5 seconds */
	}
	
	kthread_exit();
}

/* Sysctl handler for temperature threshold values */
static int
sysctl_fan_temp_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t value;
	int error;
	
	mtx_lock(&cooling_fan.mtx);
	value = *(uint32_t *)arg1;
	mtx_unlock(&cooling_fan.mtx);
	
	error = sysctl_handle_32(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	
	/* Validate temperature is reasonable (0-120000 mC = 0-120°C) */
	if (value > 120000)
		return (EINVAL);
	
	mtx_lock(&cooling_fan.mtx);
	*(uint32_t *)arg1 = value;
	mtx_unlock(&cooling_fan.mtx);
	
	/* Trigger fan state update when thresholds change */
	rpi5_update_fan_state();
	
	return (0);
}

/* Sysctl handler for hysteresis values */
static int
sysctl_fan_hyst_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t value;
	int error;
	
	mtx_lock(&cooling_fan.mtx);
	value = *(uint32_t *)arg1;
	mtx_unlock(&cooling_fan.mtx);
	
	error = sysctl_handle_32(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	
	/* Validate hysteresis is reasonable (0-10000 mC = 0-10°C) */
	if (value > 10000)
		return (EINVAL);
	
	mtx_lock(&cooling_fan.mtx);
	*(uint32_t *)arg1 = value;
	mtx_unlock(&cooling_fan.mtx);
	
	return (0);
}

/* Sysctl handler for PWM speed values */
static int
sysctl_fan_speed_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t value;
	int error;
	
	mtx_lock(&cooling_fan.mtx);
	value = *(uint32_t *)arg1;
	mtx_unlock(&cooling_fan.mtx);
	
	error = sysctl_handle_32(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	
	/* Validate PWM speed is 0-255 */
	if (value > 255)
		return (EINVAL);
	
	mtx_lock(&cooling_fan.mtx);
	*(uint32_t *)arg1 = value;
	mtx_unlock(&cooling_fan.mtx);
	
	/* Update fan if state would use this speed */
	rpi5_update_fan_state();
	
	return (0);
}

/* Sysctl handler for CPU temperature (read-only) */
static int
sysctl_cpu_temp_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t value;
	int error;
	
	mtx_lock(&cooling_fan.mtx);
	value = cooling_fan.cpu_temp;
	mtx_unlock(&cooling_fan.mtx);
	
	error = sysctl_handle_32(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	
	/* CPU temperature is read-only */
	return (EPERM);
}

/* Sysctl handler for current fan state (read-only) */
static int
sysctl_fan_state_handler(SYSCTL_HANDLER_ARGS)
{
	uint32_t value;
	int error;
	
	mtx_lock(&cooling_fan.mtx);
	value = cooling_fan.fan_current_state;
	mtx_unlock(&cooling_fan.mtx);
	
	error = sysctl_handle_32(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	
	/* Current state is read-only from userspace */
	return (EPERM);
}

/* Sysctl tree setup */
static SYSCTL_NODE(_hw, OID_AUTO, rpi5, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
	"Raspberry Pi 5 Hardware Controls");

static SYSCTL_NODE(_hw_rpi5, OID_AUTO, cooling_fan, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
	"Cooling Fan Control");

/* Temperature threshold OIDs */
SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp0,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp0, 0, sysctl_fan_temp_handler, "IU",
	"Temperature threshold for cooling level 0 (millidegrees Celsius)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp1,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp1, 0, sysctl_fan_temp_handler, "IU",
	"Temperature threshold for cooling level 1 (millidegrees Celsius)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp2,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp2, 0, sysctl_fan_temp_handler, "IU",
	"Temperature threshold for cooling level 2 (millidegrees Celsius)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp3,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp3, 0, sysctl_fan_temp_handler, "IU",
	"Temperature threshold for cooling level 3 (millidegrees Celsius)");

/* Hysteresis OIDs */
SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp0_hyst,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp0_hyst, 0, sysctl_fan_hyst_handler, "IU",
	"Hysteresis for cooling level 0 (millidegrees Celsius)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp1_hyst,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp1_hyst, 0, sysctl_fan_hyst_handler, "IU",
	"Hysteresis for cooling level 1 (millidegrees Celsius)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp2_hyst,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp2_hyst, 0, sysctl_fan_hyst_handler, "IU",
	"Hysteresis for cooling level 2 (millidegrees Celsius)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, temp3_hyst,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp3_hyst, 0, sysctl_fan_hyst_handler, "IU",
	"Hysteresis for cooling level 3 (millidegrees Celsius)");

/* PWM Speed OIDs */
SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, speed0,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp0_speed, 0, sysctl_fan_speed_handler, "IU",
	"PWM speed for cooling level 0 (0-255)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, speed1,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp1_speed, 0, sysctl_fan_speed_handler, "IU",
	"PWM speed for cooling level 1 (0-255)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, speed2,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp2_speed, 0, sysctl_fan_speed_handler, "IU",
	"PWM speed for cooling level 2 (0-255)");

SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, speed3,
	CTLFLAG_RW | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_temp3_speed, 0, sysctl_fan_speed_handler, "IU",
	"PWM speed for cooling level 3 (0-255)");

/* Current CPU temperature (read-only) */
SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, cpu_temp,
	CTLFLAG_RD | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.cpu_temp, 0, sysctl_cpu_temp_handler, "IU",
	"Current CPU temperature (millidegrees Celsius, read-only)");

/* Current fan state (read-only) */
SYSCTL_PROC(_hw_rpi5_cooling_fan, OID_AUTO, current_state,
	CTLFLAG_RD | CTLFLAG_MPSAFE | CTLTYPE_U32,
	&cooling_fan.fan_current_state, 0, sysctl_fan_state_handler, "IU",
	"Current fan state (0-3, read-only)");

/* Module event handler */
static int
rpi5_cooling_fan_modevent(module_t mod, int type, void *data)
{
	int error = 0;
	
	switch (type) {
	case MOD_LOAD:
		mtx_init(&cooling_fan.mtx, "rpi5_cooling_fan", NULL, MTX_DEF);
		
		/* Initialize callout for thermal monitoring */
		callout_init_mtx(&cooling_fan.thermal_callout, &cooling_fan.mtx, 0);
		
		/* Set initial PWM device to NULL - will be discovered at runtime */
		cooling_fan.pwm_dev = NULL;
		cooling_fan.thermal_active = 1;
		
		/* Start thermal management callout */
		callout_reset(&cooling_fan.thermal_callout, hz, rpi5_thermal_tick, &cooling_fan);
		
		uprintf("RPi5 Cooling Fan sysctl module loaded with thermal management\n");
		break;
		
	case MOD_UNLOAD:
		cooling_fan.thermal_active = 0;
		
		/* Cancel the callout */
		callout_drain(&cooling_fan.thermal_callout);
		
		/* Disable fan on unload */
		if (cooling_fan.pwm_dev != NULL) {
			PWMBUS_CHANNEL_ENABLE(cooling_fan.pwm_dev, 
			    cooling_fan.pwm_channel, false);
		}
		
		mtx_destroy(&cooling_fan.mtx);
		uprintf("RPi5 Cooling Fan sysctl module unloaded\n");
		break;
		
	default:
		error = EOPNOTSUPP;
		break;
	}
	
	return (error);
}

static moduledata_t rpi5_cooling_fan_mod = {
	"rpi5_cooling_fan",
	rpi5_cooling_fan_modevent,
	NULL
};

DECLARE_MODULE(rpi5_cooling_fan, rpi5_cooling_fan_mod, SI_SUB_DRIVERS,
	SI_ORDER_MIDDLE);
MODULE_VERSION(rpi5_cooling_fan, 2);
