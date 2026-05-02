/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * rp1_gpio — RP1 GPIO / Pinctrl driver — private header
 *
 * Hardware reference: RP-008370-DS-1 (RP1 datasheet)
 * Driver pattern:     sys/arm/broadcom/bcm2835/bcm2835_gpio.c
 * FDT spec:           RP1_GPIO_spec.md §3
 *
 * Attach strategy note (M1):
 *   The Pi 5 boots with ACPI, not FDT bus enumeration, so there is no
 *   simplebus in the device tree.  The driver registers under nexus and
 *   uses device_identify to create its own device_t after locating the
 *   gpio@d0000 FDT node.  Registers are mapped via pmap_mapdev_attr,
 *   mirroring bcm2712.c and rp1_eth_cfg.c.
 */

#ifndef _RP1_GPIO_VAR_H_
#define _RP1_GPIO_VAR_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <machine/bus.h>

/* -----------------------------------------------------------------------
 * Pin/bank constants
 * ----------------------------------------------------------------------- */
#define RP1_NUM_GPIOS	54
#define RP1_NUM_BANKS	3

/* Bank for a given pin number */
#define RP1_GPIO_BANK(pin) \
    ((pin) < 28 ? 0 : (pin) < 34 ? 1 : 2)

/* Pin index within its bank */
#define RP1_GPIO_PIN_IN_BANK(pin) \
    ((pin) < 28 ? (pin) : (pin) < 34 ? (pin) - 28 : (pin) - 34)

/* -----------------------------------------------------------------------
 * Physical base addresses (CPU-side via BCM2712 PCIe2 outbound window)
 * Confirmed by live DTB gpio@d0000 reg entries — see RP1_GPIO_spec.md §3.1
 * ----------------------------------------------------------------------- */
#define RP1_IO_BANK_BASE_PHYS	0x1f000d0000UL	/* IO_BANK0..2, size 0xc000 */
#define RP1_SYS_RIO_BASE_PHYS	0x1f000e0000UL	/* SYS_RIO0..2, size 0xc000 */
#define RP1_PADS_BANK_BASE_PHYS	0x1f000f0000UL	/* PADS_BANK0..2, size 0xc000 */
#define RP1_GPIO_REGION_SIZE	0xc000		/* three 0x4000 banks per region */

/* -----------------------------------------------------------------------
 * Bank offset — same layout for IO_BANK, RIO, and PADS regions
 * ----------------------------------------------------------------------- */
#define RP1_BANK_OFFSET(bank)	((uintptr_t)(bank) * 0x4000)

/* -----------------------------------------------------------------------
 * IO_BANK register offsets (within a bank's 0x4000 slice)
 * Each pin occupies an 8-byte slot: STATUS at +0, CTRL at +4
 * ----------------------------------------------------------------------- */
#define RP1_IO_STATUS(idx)	((uintptr_t)(idx) * 8)
#define RP1_IO_CTRL(idx)	((uintptr_t)(idx) * 8 + 4)

/* IO_BANK CTRL register bit fields (RP-008370-DS-1 §3.1.4 Table 8) */
#define RP1_CTRL_FUNCSEL_MASK	0x1fu		/* bits [4:0] */
#define RP1_CTRL_OUTOVER_MASK	(3u << 12)	/* bits [13:12]: output override */
#define RP1_CTRL_OEOVER_MASK	(3u << 14)	/* bits [15:14]: output-enable override */
#define RP1_CTRL_IRQRESET	(1u << 28)	/* write-1: clear pending IRQ latch */

/* IO_BANK STATUS register: latched interrupt events in bits [27:20] */
#define RP1_STATUS_IRQ_MASK	(0xffu << 20)

/* FUNCSEL values */
#define RP1_FSEL_ALT0	0x00u	/* peripheral function 0 */
#define RP1_FSEL_ALT1	0x01u
#define RP1_FSEL_ALT2	0x02u
#define RP1_FSEL_ALT3	0x03u
#define RP1_FSEL_ALT4	0x04u
#define RP1_FSEL_GPIO	0x05u	/* software GPIO via SYS_RIO */
#define RP1_FSEL_ALT6	0x06u
#define RP1_FSEL_ALT7	0x07u
#define RP1_FSEL_ALT8	0x08u
#define RP1_FSEL_NONE	0x1fu	/* no connect / hi-Z */

/* OUTOVER / OEOVER 2-bit field values (same encoding for both fields) */
#define RP1_OVER_PERI	0u	/* pass through from peripheral / RIO */
#define RP1_OVER_INV	1u	/* invert */
#define RP1_OVER_LOW	2u	/* force low / force-disable OE */
#define RP1_OVER_HIGH	3u	/* force high / force-enable OE */

/* -----------------------------------------------------------------------
 * SYS_RIO register layout (within each bank's 0x4000 slice)
 *
 * Four atomic-access aliases at 0x1000-byte steps within the bank slice:
 *   RW  (bank_offset+0x0000): read/write directly
 *   XOR (bank_offset+0x1000): write toggles bits
 *   SET (bank_offset+0x2000): write sets bits
 *   CLR (bank_offset+0x3000): write clears bits
 *
 * Registers within each alias:
 *   +0x00 OUT     — output level (one bit per pin in bank)
 *   +0x04 OE      — output-enable
 *   +0x08 IN      — raw pad input
 *   +0x0C IN_SYNC — synchronised pad input (use for pin_get)
 * ----------------------------------------------------------------------- */
#define RP1_RIO_RW	0x0000u
#define RP1_RIO_XOR	0x1000u
#define RP1_RIO_SET	0x2000u
#define RP1_RIO_CLR	0x3000u

#define RP1_RIO_OUT	0x00u
#define RP1_RIO_OE	0x04u
#define RP1_RIO_IN	0x08u
#define RP1_RIO_IN_SYNC	0x0cu

/* Full byte offset into the RIO region for a given bank / alias / register */
#define RP1_RIO_OFF(bank, alias, reg) \
    (RP1_BANK_OFFSET(bank) + (uintptr_t)(alias) + (uintptr_t)(reg))

/* -----------------------------------------------------------------------
 * PADS_BANK register layout
 * First word in each bank = VOLTAGE_SELECT; pin N pad = (N+1)*4 within bank.
 * ----------------------------------------------------------------------- */
#define RP1_PAD_OFF(bank, idx) \
    (RP1_BANK_OFFSET(bank) + ((uintptr_t)(idx) + 1) * 4)

/* PADS register bit fields */
#define RP1_PAD_OD		(1u << 7)	/* open-drain */
#define RP1_PAD_DRIVE_MASK	(3u << 5)	/* 0=2mA 1=4mA 2=8mA 3=12mA */
#define RP1_PAD_DRIVE_SHIFT	5
#define RP1_PAD_PUE		(1u << 4)	/* pull-up enable */
#define RP1_PAD_PDE		(1u << 3)	/* pull-down enable */
#define RP1_PAD_SCHMITT		(1u << 2)
#define RP1_PAD_SLEWFAST	(1u << 1)
#define RP1_PAD_IE		(1u << 0)	/* input enable */

/* -----------------------------------------------------------------------
 * Softc
 *
 * Registers are accessed via KVA pointers obtained from pmap_mapdev_attr
 * rather than bus_alloc_resource, because the device is created synthetically
 * under nexus (ACPI-booted system; no simplebus enumerates the FDT tree).
 * ----------------------------------------------------------------------- */
struct rp1_gpio_softc {
	device_t		 sc_dev;
	device_t		 sc_busdev;		/* gpiobus child */
	struct mtx		 sc_mtx;		/* spin; protects CTRL/PADS RMW */
	void			*sc_io_kva;		/* IO_BANK KVA (0xc000 bytes) */
	void			*sc_rio_kva;		/* SYS_RIO KVA (0xc000 bytes) */
	void			*sc_pad_kva;		/* PADS_BANK KVA (0xc000 bytes) */
	u_int			 sc_npins;		/* always RP1_NUM_GPIOS */
	struct gpio_pin		 sc_pins[RP1_NUM_GPIOS];
};

#define RP1_GPIO_LOCK(sc)	 mtx_lock_spin(&(sc)->sc_mtx)
#define RP1_GPIO_UNLOCK(sc)	 mtx_unlock_spin(&(sc)->sc_mtx)
#define RP1_GPIO_LOCK_ASSERT(sc) mtx_assert(&(sc)->sc_mtx, MA_OWNED)

/* -----------------------------------------------------------------------
 * Inline register accessors — KVA pointer style, matching rp1_eth_cfg.c
 * and bcm2712.c.  Used by rp1_gpio.c; shared with rp1_gpio_func.c (M2).
 * ----------------------------------------------------------------------- */
static inline uint32_t
rp1_io_read(struct rp1_gpio_softc *sc, u_int pin)
{
	int b = RP1_GPIO_BANK(pin);
	u_int i = RP1_GPIO_PIN_IN_BANK(pin);
	uintptr_t off = RP1_BANK_OFFSET(b) + RP1_IO_CTRL(i);
	return (*(volatile uint32_t *)((uintptr_t)sc->sc_io_kva + off));
}

static inline void
rp1_io_write(struct rp1_gpio_softc *sc, u_int pin, uint32_t val)
{
	int b = RP1_GPIO_BANK(pin);
	u_int i = RP1_GPIO_PIN_IN_BANK(pin);
	uintptr_t off = RP1_BANK_OFFSET(b) + RP1_IO_CTRL(i);
	*(volatile uint32_t *)((uintptr_t)sc->sc_io_kva + off) = val;
}

static inline uint32_t
rp1_rio_read(struct rp1_gpio_softc *sc, int bank, uint32_t alias, uint32_t reg)
{
	uintptr_t off = RP1_RIO_OFF(bank, alias, reg);
	return (*(volatile uint32_t *)((uintptr_t)sc->sc_rio_kva + off));
}

static inline void
rp1_rio_write(struct rp1_gpio_softc *sc, int bank, uint32_t alias,
    uint32_t reg, uint32_t val)
{
	uintptr_t off = RP1_RIO_OFF(bank, alias, reg);
	*(volatile uint32_t *)((uintptr_t)sc->sc_rio_kva + off) = val;
}

static inline uint32_t
rp1_pad_read(struct rp1_gpio_softc *sc, u_int pin)
{
	int b = RP1_GPIO_BANK(pin);
	u_int i = RP1_GPIO_PIN_IN_BANK(pin);
	uintptr_t off = RP1_PAD_OFF(b, i);
	return (*(volatile uint32_t *)((uintptr_t)sc->sc_pad_kva + off));
}

static inline void
rp1_pad_write(struct rp1_gpio_softc *sc, u_int pin, uint32_t val)
{
	int b = RP1_GPIO_BANK(pin);
	u_int i = RP1_GPIO_PIN_IN_BANK(pin);
	uintptr_t off = RP1_PAD_OFF(b, i);
	*(volatile uint32_t *)((uintptr_t)sc->sc_pad_kva + off) = val;
}

#endif /* _RP1_GPIO_VAR_H_ */
