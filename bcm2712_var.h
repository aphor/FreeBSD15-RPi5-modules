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

/*
 * RP1 PWM1 controller physical address, via pcie2 outbound window.
 * Derivation:
 *   pcie2 ranges: PCIe addr 0x0 -> CPU phys 0x1f_00000000
 *   RP1 bus base: child 0xc0_40000000 -> PCIe addr 0x0
 *   pwm@9c000 reg: 0xc0_4009c000 (offset 0x9c000 from RP1 base)
 *   CPU physical = 0x1f_00000000 + 0x9c000 = 0x1f0009c000
 *
 * The fan (GPIO45) is driven by PWM1 channel 3, per DTB:
 *   cooling_fan { pwms = <pwm1_phandle 3 41566ns inverted>; }
 */
#define RP1_PWM1_BASE_PHYS	0x1f0009c000UL
#define RP1_PWM_MAP_SIZE	0x1000

/* RP1 PWM register offsets (per Linux pwm-rp1.c / RP1 datasheet). */
#define RP1_PWM_GLOBAL_CTRL	0x000
#define RP1_PWM_CHAN_CTRL(x)	(0x014 + (x) * 16)	/* ch3 = 0x044 */
#define RP1_PWM_RANGE(x)	(0x018 + (x) * 16)	/* ch3 = 0x048 */
#define RP1_PWM_DUTY(x)		(0x020 + (x) * 16)	/* ch3 = 0x050 */

#define RP1_PWM_CHAN_DEFAULT	0x101u		/* FIFO_POP | trailing-edge M/S */
#define RP1_PWM_CHAN_ENABLE(x)	(1u << (x))	/* enable bit in GLOBAL_CTRL */
#define RP1_PWM_POLARITY	(1u << 3)	/* inverted polarity in CHAN_CTRL */
#define RP1_PWM_SET_UPDATE	(1u << 31)	/* commit writes via GLOBAL_CTRL */

#define RP1_PWM_CLK_HZ		50000000u	/* 50 MHz per DTB assigned-clock-rates */
#define RP1_PWM_CLK_PERIOD_NS	20u		/* 1e9 / 50e6 */

/*
 * RP1 GPIO controller (bank 0 resource covers all CTRL registers).
 * Physical: pcie2 outbound base 0x1f_00000000 + RP1 GPIO offset 0xd0000.
 * GPIO CTRL registers:
 *   Bank 0 (GPIO 0-27):  offset 0x0000 + pin*8 + 4
 *   Bank 1 (GPIO 28-33): offset 0x4000 + (pin-28)*8 + 4
 *   Bank 2 (GPIO 34-53): offset 0x8000 + (pin-34)*8 + 4
 * FUNCSEL is bits [4:0] in CTRL; ALT0=0x00, GPIO=0x05 (from pinctrl-rp1.c).
 * Fan PWM: GPIO45 → ALT0 = pwm1 (per PIN(45, pwm1, ...) in pinctrl-rp1.c).
 */
#define RP1_GPIO_BASE_PHYS	0x1f000d0000UL
#define RP1_GPIO_MAP_SIZE	0xc000
#define RP1_GPIO_CTRL_OFFSET(pin) \
    (((pin) < 28 ? 0x0000 : (pin) < 34 ? 0x4000 : 0x8000) + \
     ((pin) < 28 ? (pin) : (pin) < 34 ? (pin)-28 : (pin)-34) * 8 + 4)
#define RP1_GPIO_FUNCSEL_MASK	0x1fu
#define RP1_GPIO_FSEL_ALT0	0x00u		/* hardware peripheral function 0 */
#define RP1_GPIO_FAN_PIN	45		/* GPIO45 → PWM1 channel 3 → fan */

/* BCM2712 controller structure */
struct bcm2712_softc {
	struct mtx mtx;

	/* AVS thermal sensor virtual address mapping */
	void *avs_vaddr;			/* Virtual address of AVS monitor */
	int avs_mapped;				/* AVS memory mapped successfully */

	/* RP1 PWM controller virtual address mapping */
	void *pwm_vaddr;			/* Virtual address of PWM controller */
	int pwm_mapped;				/* PWM memory mapped successfully */

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