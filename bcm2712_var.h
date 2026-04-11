/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * BCM2712 Common Hardware Support - Header File
 */

#ifndef _BCM2712_VAR_H_
#define _BCM2712_VAR_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/time.h>
#include <machine/bus.h>

/* Number of PWM channels on RP1 */
#define BCM2712_PWM_NCHANNELS	4

/* BCM2712 PWM channel structure */
struct bcm2712_pwm_channel {
	uint32_t period;	/* Period in nanoseconds */
	uint32_t duty_a;	/* Duty cycle A (nanoseconds) */
	uint32_t duty_b;	/* Duty cycle B (nanoseconds) */
	uint32_t flags;		/* PWM flags (polarity, etc) */
	bool enabled;		/* Channel enabled */
};

/* BCM2712 controller structure */
struct bcm2712_softc {
	struct mtx mtx;

	/* AVS thermal sensor virtual address mapping */
	void *avs_vaddr;			/* Virtual address of AVS monitor */
	int avs_mapped;				/* AVS memory mapped successfully */

	/* PWM channels (4 available on RP1) */
	struct bcm2712_pwm_channel channels[BCM2712_PWM_NCHANNELS];

	/* Thermal sensor support */
	struct mtx thermal_mtx;			/* Protect thermal reads */
	struct callout thermal_callout;		/* Periodic update timer */
	uint32_t cached_temp_mc;		/* Cached milli-°C value */
	time_t last_update;			/* Timestamp of last read */
	struct sysctl_ctx_list sysctl_ctx;	/* sysctl context */
	struct sysctl_oid *sysctl_tree;		/* sysctl tree root */
};

/* Function prototypes for other modules */
int bcm2712_read_cpu_temp(uint32_t *temp);
struct bcm2712_softc *bcm2712_get_softc(void);
int bcm2712_pwm_set_config(u_int channel, u_int period, u_int duty);
int bcm2712_pwm_enable(u_int channel, bool enable);

#endif /* _BCM2712_VAR_H_ */