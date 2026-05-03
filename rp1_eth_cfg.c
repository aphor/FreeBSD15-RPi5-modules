/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * rp1_eth — Milestone 1: eth_cfg bring-up and link observation
 *
 * This module performs the minimum work needed to observe RGMII link state
 * on the Raspberry Pi 5's on-board Cadence GEM_GXL Ethernet MAC without
 * touching the MAC DMA paths.  It validates:
 *   - pcie2 outbound window MMIO mapping (eth_cfg at 0x1f_00104000)
 *   - FDT metadata extraction (MAC addr, phy-mode, PHY reset GPIO)
 *   - BCM PHY reset via GPIO 32 (active-low)
 *   - eth_cfg.STATUS.RGMII_LINK_STATUS toggling on cable plug/unplug
 *
 * Exit criteria: plug/unplug the Ethernet cable and observe
 *   sysctl hw.rp1_eth.cfg.status changing.
 *
 * Milestone 2 will fork if_cgem.c into rp1_eth.c and bring up the full
 * network stack.  See if_gem-PLAN.md for the complete three-milestone plan.
 *
 * References:
 *   RP-008370-DS-1 §7  — eth_cfg register map
 *   bcm2712.c          — pmap_mapdev_attr + GPIO FUNCSEL pattern
 *   rp1_eth_var.h      — physical addresses, register definitions
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/endian.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>

#include <dev/ofw/openfirm.h>

#include "rp1_eth_var.h"
#include "bcm2712_var.h"		/* RP1_GPIO_* constants */

MALLOC_DEFINE(M_RP1ETH, "rp1_eth", "RP1 Ethernet driver memory");

static struct rp1_eth_softc *rp1_eth_sc = NULL;

/* -----------------------------------------------------------------------
 * FDT helpers
 * ----------------------------------------------------------------------- */

/*
 * Return non-zero if FDT node @node has @compat in its compatible list.
 * The compatible property is a NUL-separated list of strings.
 */
static int
rp1_eth_node_is_compatible(phandle_t node, const char *compat)
{
	char buf[256];
	const char *s, *end;
	int len;

	len = OF_getprop(node, "compatible", buf, sizeof(buf) - 1);
	if (len <= 0)
		return (0);
	buf[len] = '\0';

	s = buf;
	end = buf + len;
	while (s < end) {
		if (strcmp(s, compat) == 0)
			return (1);
		s += strlen(s) + 1;
	}
	return (0);
}

/*
 * Depth-first search for the first FDT node with @compat in its compatible.
 * Returns 0 if not found.
 */
static phandle_t
rp1_eth_fdt_find_compat(phandle_t start, const char *compat)
{
	phandle_t child, found;

	if (rp1_eth_node_is_compatible(start, compat))
		return (start);

	for (child = OF_child(start); child != 0; child = OF_peer(child)) {
		found = rp1_eth_fdt_find_compat(child, compat);
		if (found != 0)
			return (found);
	}
	return (0);
}

/*
 * Read FDT metadata from the raspberrypi,rp1-gem node into sc.
 *
 * Reads:
 *   local-mac-address  (6 bytes)
 *   phy-mode           (string, must equal "rgmii-id" for this milestone)
 *   phy-handle         → lookup ethernet-phy child → read reg (MDIO addr)
 *   phy-reset-gpios    (phandle, gpio-num, flags)
 *   phy-reset-duration (u32, milliseconds — should be 5)
 *
 * Returns 0 on success, errno on failure.
 */
static int
rp1_eth_fdt_read_metadata(struct rp1_eth_softc *sc)
{
	phandle_t root, gem_node, phy_node;
	uint8_t mac[6];
	uint32_t cells[3];
	uint32_t phandle_val;
	int len;

	root = OF_finddevice("/");
	if (root == 0) {
		printf("rp1_eth: FDT root not found\n");
		return (ENODEV);
	}

	/* Verify this is a Pi 5 variant */
	if (!rp1_eth_node_is_compatible(root, RPI5_COMPAT_5B) &&
	    !rp1_eth_node_is_compatible(root, RPI5_COMPAT_CM5)) {
		printf("rp1_eth: root compatible does not match Pi 5 "
		    "(%s or %s) — refusing attach\n",
		    RPI5_COMPAT_5B, RPI5_COMPAT_CM5);
		return (ENXIO);
	}

	/* Find the GEM node */
	gem_node = rp1_eth_fdt_find_compat(root, RP1_ETH_COMPAT_PRIMARY);
	if (gem_node == 0) {
		printf("rp1_eth: FDT node '%s' not found\n",
		    RP1_ETH_COMPAT_PRIMARY);
		return (ENODEV);
	}
	printf("rp1_eth: found FDT node '%s'\n", RP1_ETH_COMPAT_PRIMARY);

	/* local-mac-address (6 raw bytes, not a string) */
	len = OF_getprop(gem_node, "local-mac-address", mac, sizeof(mac));
	if (len == 6) {
		memcpy(sc->mac_addr, mac, 6);
		printf("rp1_eth: MAC %02x:%02x:%02x:%02x:%02x:%02x "
		    "(firmware-stamped)\n",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	} else {
		printf("rp1_eth: local-mac-address missing or wrong length "
		    "(%d), using zero\n", len);
		memset(sc->mac_addr, 0, 6);
	}

	/* phy-mode (NUL-terminated string) */
	len = OF_getprop(gem_node, "phy-mode",
	    sc->phy_mode, sizeof(sc->phy_mode) - 1);
	if (len > 0) {
		sc->phy_mode[len] = '\0';
		printf("rp1_eth: phy-mode = \"%s\"\n", sc->phy_mode);
	} else {
		printf("rp1_eth: phy-mode missing\n");
		sc->phy_mode[0] = '\0';
	}

	/*
	 * Milestone 1 only supports rgmii-id.
	 * rgmii-id means the BCM PHY applies both TX and RX delays internally.
	 * We MUST NOT set CLKGEN.{TXCLKDELEN,RXCLKDELEN} — doing so would add
	 * extra delay and cause CRC errors at 1 Gbps.
	 */
	if (strcmp(sc->phy_mode, RP1_ETH_PHY_MODE_RGMII_ID) != 0) {
		printf("rp1_eth: phy-mode \"%s\" != \"%s\"; "
		    "refusing attach (can be relaxed in milestone 2)\n",
		    sc->phy_mode, RP1_ETH_PHY_MODE_RGMII_ID);
		return (ENXIO);
	}

	/*
	 * phy-handle is a phandle to the ethernet-phy child node.
	 * Read the phandle value, resolve it to a node, then read .reg
	 * for the MDIO address.
	 */
	len = OF_getprop(gem_node, "phy-handle", &phandle_val,
	    sizeof(phandle_val));
	if (len == sizeof(phandle_val)) {
		phy_node = OF_node_from_xref(be32toh(phandle_val));
		if (phy_node != 0) {
			uint32_t phy_reg;
			len = OF_getprop(phy_node, "reg", &phy_reg,
			    sizeof(phy_reg));
			if (len == sizeof(phy_reg)) {
				sc->phy_addr = be32toh(phy_reg);
				printf("rp1_eth: PHY MDIO addr = %u\n",
				    sc->phy_addr);
			}
		}
	}

	/*
	 * phy-reset-gpios = <phandle gpio_num flags>
	 * flags bit 0: active-low (GPIO_ACTIVE_LOW = 1)
	 */
	len = OF_getprop(gem_node, "phy-reset-gpios", cells, sizeof(cells));
	if (len >= (int)(3 * sizeof(uint32_t))) {
		/* cells[0] = phandle (not used here — we hardcode the GPIO) */
		sc->phy_reset_gpio  = be32toh(cells[1]);
		sc->phy_reset_flags = be32toh(cells[2]);
		printf("rp1_eth: phy-reset-gpios = GPIO%u, flags=0x%x (%s)\n",
		    sc->phy_reset_gpio, sc->phy_reset_flags,
		    (sc->phy_reset_flags & 1) ? "active-low" : "active-high");
	} else {
		printf("rp1_eth: phy-reset-gpios missing — "
		    "using default GPIO%u active-low\n",
		    RP1_ETH_PHY_RESET_GPIO);
		sc->phy_reset_gpio  = RP1_ETH_PHY_RESET_GPIO;
		sc->phy_reset_flags = 1; /* active-low default */
	}

	return (0);
}

/* -----------------------------------------------------------------------
 * PHY reset
 *
 * Uses RP1 GPIO CTRL register OUTOVER bits to force the reset pin
 * LOW (assert) then HIGH (deassert) without needing the SIO registers.
 *
 * GPIO32 is in bank 1 (pins 28-33), bank base offset 0x4000.
 * CTRL register = GPIO_BASE + 0x4000 + (32-28)*8 + 4 = GPIO_BASE + 0x4024
 *
 * CTRL bit fields (verify against RP-008370-DS-1 §5 GPIO_CTRL):
 *   [4:0]  FUNCSEL  — leave unchanged (or set 5 for GPIO-software mode)
 *   [9:8]  OUTOVER  — 2=force-low, 3=force-high
 *   [11:10] OEOVER  — 3=force output-enable ON
 *
 * We map GPIO registers, pulse the reset, then unmap.
 * ----------------------------------------------------------------------- */
static void
rp1_eth_phy_reset(struct rp1_eth_softc *sc)
{
	void *gpio_map, *pads_map;
	volatile uint32_t *ctrl_reg, *pad_reg;
	uint32_t ctrl_val, pad_old;
	uint32_t gpio_pin = sc->phy_reset_gpio;

	/*
	 * Map PADS_BANK1 to change GPIO32 from open-drain to push-pull.
	 *
	 * Firmware leaves GPIO32 PAD with OD=1 (open-drain).  In open-drain
	 * mode the PMOS transistor is disabled, so our OUTOVER=force-HIGH only
	 * releases our NMOS — the pin level is determined by the external pull-up
	 * vs whatever BCM54213PE drives.  BCM can hold RESET_N LOW indefinitely
	 * with its own open-drain assertion, overwhelming the weak integrated
	 * pull-up (~50 kΩ, ~66 µA) with its internal reset circuit (~mA-range).
	 *
	 * Changing to OD=0 (push-pull) enables the PMOS (8 mA source), which
	 * overrides BCM's open-drain assertion and reliably deasserts RESET_N.
	 * Linux's gpiod framework does the equivalent automatically.
	 */
	pads_map = pmap_mapdev_attr(RP1_PADS_BANK1_BASE_PHYS, RP1_PADS_MAP_SIZE,
	    VM_MEMATTR_DEVICE);
	if (pads_map != NULL) {
		pad_reg = (volatile uint32_t *)
		    ((uintptr_t)pads_map + RP1_GPIO32_PAD_OFFSET);
		pad_old = *pad_reg;
		/* OD=0 (push-pull) + IE=1 (input enabled so we can verify level) */
		*pad_reg = RP1_PAD_PUSHPULL_HIGH_IE;
		printf("rp1_eth: GPIO%u PAD 0x%02x→0x%02x (OD cleared for push-pull)\n",
		    gpio_pin, pad_old, *pad_reg);
		pmap_unmapdev(pads_map, RP1_PADS_MAP_SIZE);
	} else {
		printf("rp1_eth: cannot map PADS_BANK1 — reset deassert may fail\n");
	}

	gpio_map = pmap_mapdev_attr(RP1_GPIO_BASE_PHYS, RP1_GPIO_MAP_SIZE,
	    VM_MEMATTR_DEVICE);
	if (gpio_map == NULL) {
		printf("rp1_eth: cannot map GPIO registers for PHY deassert\n");
		return;
	}

	ctrl_reg = (volatile uint32_t *)
	    ((uintptr_t)gpio_map + RP1_GPIO_CTRL_OFFSET(gpio_pin));
	ctrl_val = *ctrl_reg;
	printf("rp1_eth: GPIO%u CTRL = 0x%08x "
	    "(OUTOVER=%u OEOVER=%u)\n", gpio_pin, ctrl_val,
	    (ctrl_val & RP1_GPIO_OUTOVER_MASK) >> 12,
	    (ctrl_val & RP1_GPIO_OEOVER_MASK)  >> 14);

	/* Assert RESET_N LOW (hardware reset) for the defined pulse width */
	ctrl_val &= ~(RP1_GPIO_OUTOVER_MASK | RP1_GPIO_OEOVER_MASK);
	ctrl_val |= RP1_GPIO_OEOVER_FORCE_EN | RP1_GPIO_OUTOVER_LOW;
	*ctrl_reg = ctrl_val;
	printf("rp1_eth: GPIO%u RESET_N asserted LOW (CTRL=0x%08x)\n",
	    gpio_pin, ctrl_val);

	DELAY(RP1_ETH_PHY_RESET_PULSE_US);   /* 5 ms hardware reset pulse */

	/* Deassert RESET_N HIGH (push-pull with OD=0 → drives pin to VCC) */
	ctrl_val &= ~RP1_GPIO_OUTOVER_MASK;
	ctrl_val |= RP1_GPIO_OUTOVER_HIGH;
	*ctrl_reg = ctrl_val;
	printf("rp1_eth: GPIO%u RESET_N deasserted push-pull (CTRL=0x%08x)\n",
	    gpio_pin, ctrl_val);

	pmap_unmapdev(gpio_map, RP1_GPIO_MAP_SIZE);

	/* Wait for BCM54213PE to complete internal power-on reset (~150 ms) */
	pause("rp1_eth_phy",
	    ((uint64_t)RP1_ETH_PHY_MDIO_READY_MS * hz + 999) / 1000);
}

/* -----------------------------------------------------------------------
 * sysctl handlers
 * ----------------------------------------------------------------------- */

/*
 * MDIO clause-22 read.  Caller must hold no locks (uses DELAY polling).
 * MDIO_EN must be set in NET_CTRL before calling.
 */
static uint16_t
rp1_eth_mdio_read(struct rp1_eth_softc *sc, int phy, int reg)
{
	uint32_t frame;
	int i;

	/* Wait for any in-progress management frame to complete */
	for (i = 0; i < 1000; i++) {
		if (MAC_RD4(sc, GEM_NET_STATUS) & GEM_NET_STATUS_MDIO_IDLE)
			break;
		DELAY(10);
	}

	frame = GEM_MAINT_SOF | GEM_MAINT_OP_RD |
	    GEM_MAINT_PHY(phy) | GEM_MAINT_REG(reg) | GEM_MAINT_TA;
	MAC_WR4(sc, GEM_PHY_MAINT, frame);

	/*
	 * The GEM needs a few internal cycles after the PHY_MAINT write to
	 * actually start the management transaction and clear phy_mgmt_idle
	 * (bit 2 of NET_STATUS).  Without this delay the poll loop below
	 * sees bit 2 still set from the pre-write idle state and returns
	 * before the transaction completes, reading 0xffff (undriven bus).
	 * One MDC period at ÷96 ≈ 480 ns; wait 4 periods = ~2 µs to be safe.
	 */
	DELAY(10);

	/* Wait for transaction to complete (phy_mgmt_idle = 1 again) */
	for (i = 0; i < 1000; i++) {
		if (MAC_RD4(sc, GEM_NET_STATUS) & GEM_NET_STATUS_MDIO_IDLE)
			break;
		DELAY(10);
	}

	return ((uint16_t)MAC_RD4(sc, GEM_PHY_MAINT));
}

/*
 * MDIO clause-22 write.  Caller must hold no locks (uses DELAY polling).
 * MDIO_EN must be set in NET_CTRL before calling.
 */
static void
rp1_eth_mdio_write(struct rp1_eth_softc *sc, int phy, int reg, uint16_t data)
{
	uint32_t frame;
	int i;

	/* Wait for any in-progress management frame to complete */
	for (i = 0; i < 1000; i++) {
		if (MAC_RD4(sc, GEM_NET_STATUS) & GEM_NET_STATUS_MDIO_IDLE)
			break;
		DELAY(10);
	}

	frame = GEM_MAINT_SOF | GEM_MAINT_OP_WR |
	    GEM_MAINT_PHY(phy) | GEM_MAINT_REG(reg) | GEM_MAINT_TA |
	    GEM_MAINT_DATA(data);
	MAC_WR4(sc, GEM_PHY_MAINT, frame);
	DELAY(10);

	/* Wait for transaction to complete */
	for (i = 0; i < 1000; i++) {
		if (MAC_RD4(sc, GEM_NET_STATUS) & GEM_NET_STATUS_MDIO_IDLE)
			break;
		DELAY(10);
	}
}

/* Read a single GEM MAC register (arg2 = register offset). */
static int
rp1_eth_mac_reg_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rp1_eth_softc *sc = arg1;
	uint32_t offset = arg2;
	uint32_t val;

	if (sc == NULL || !sc->mac_mapped)
		return (ENODEV);

	val = MAC_RD4(sc, offset);
	return (sysctl_handle_int(oidp, &val, 0, req));
}

/* Read a single eth_cfg register (arg2 = register offset). */
static int
rp1_eth_cfg_reg_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rp1_eth_softc *sc = arg1;
	uint32_t offset = arg2;
	uint32_t val;

	if (sc == NULL || !sc->cfg_mapped)
		return (ENODEV);

	val = CFG_RD4(sc, offset);
	return (sysctl_handle_int(oidp, &val, 0, req));
}

/*
 * Formatted STATUS decode: shows symbolic link state rather than raw hex.
 * Useful for quick cable-event observation without external decoding.
 */
static int
rp1_eth_cfg_status_fmt_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rp1_eth_softc *sc = arg1;
	uint32_t s;
	char buf[128];
	static const char * const speed_str[] = { "10M", "100M", "1G", "?" };

	if (sc == NULL || !sc->cfg_mapped)
		return (ENODEV);

	s = CFG_RD4(sc, ETH_CFG_STATUS);
	snprintf(buf, sizeof(buf),
	    "raw=0x%08x link=%s speed=%s duplex=%s%s%s",
	    s,
	    (s & ETH_CFG_STATUS_LINK)  ? "UP"   : "DOWN",
	    speed_str[(s & ETH_CFG_STATUS_SPEED_MASK) >> ETH_CFG_STATUS_SPEED_SHIFT],
	    (s & ETH_CFG_STATUS_DUPLEX) ? "full" : "half",
	    (s & ETH_CFG_STATUS_AWLEN_ILL) ? " AWLEN_ERR" : "",
	    (s & ETH_CFG_STATUS_ARLEN_ILL) ? " ARLEN_ERR" : "");

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* -----------------------------------------------------------------------
 * Module event handler
 * ----------------------------------------------------------------------- */
static int
rp1_eth_modevent(module_t mod __unused, int event, void *arg __unused)
{
	struct rp1_eth_softc *sc;
	struct sysctl_oid *tree, *cfg_tree, *mac_tree;
	int error;

	switch (event) {
	case MOD_LOAD:
		sc = malloc(sizeof(*sc), M_RP1ETH, M_WAITOK | M_ZERO);

		mtx_init(&sc->sc_mtx, "rp1_eth", NULL, MTX_DEF);
		sysctl_ctx_init(&sc->sysctl_ctx);

		/*
		 * Step 1: Walk FDT, extract metadata, validate phy-mode.
		 */
		error = rp1_eth_fdt_read_metadata(sc);
		if (error != 0) {
			sysctl_ctx_free(&sc->sysctl_ctx);
			mtx_destroy(&sc->sc_mtx);
			free(sc, M_RP1ETH);
			return (error);
		}

		/*
		 * Step 2a: Map GEM MAC register window.
		 * We need a minimal write to GEM_NET_CTRL to enable RXEN (bit 2),
		 * which activates the GEM's RGMII receive state machine.  Without
		 * RXEN=1 the GEM's RGMII sampler is held at reset and
		 * eth_cfg.STATUS is frozen at 0 regardless of cable state.
		 */
		sc->mac_kva = pmap_mapdev_attr(RP1_ETH_MAC_BASE_PHYS,
		    RP1_ETH_MAC_MAP_SIZE, VM_MEMATTR_DEVICE);
		if (sc->mac_kva == NULL) {
			printf("rp1_eth: cannot map MAC at 0x%lx\n",
			    (unsigned long)RP1_ETH_MAC_BASE_PHYS);
			sysctl_ctx_free(&sc->sysctl_ctx);
			mtx_destroy(&sc->sc_mtx);
			free(sc, M_RP1ETH);
			return (ENXIO);
		}
		sc->mac_mapped = 1;
		printf("rp1_eth: GEM MAC mapped at phys 0x%lx KVA %p\n",
		    (unsigned long)RP1_ETH_MAC_BASE_PHYS, sc->mac_kva);

		/*
		 * Step 2b: Map eth_cfg register window.
		 * Using VM_MEMATTR_DEVICE (cache-inhibited) — same as bcm2712.c.
		 */
		sc->cfg_kva = pmap_mapdev_attr(RP1_ETH_CFG_BASE_PHYS,
		    RP1_ETH_CFG_MAP_SIZE, VM_MEMATTR_DEVICE);
		if (sc->cfg_kva == NULL) {
			printf("rp1_eth: cannot map eth_cfg at 0x%lx\n",
			    (unsigned long)RP1_ETH_CFG_BASE_PHYS);
			pmap_unmapdev(sc->mac_kva, RP1_ETH_MAC_MAP_SIZE);
			sysctl_ctx_free(&sc->sysctl_ctx);
			mtx_destroy(&sc->sc_mtx);
			free(sc, M_RP1ETH);
			return (ENXIO);
		}
		sc->cfg_mapped = 1;
		printf("rp1_eth: eth_cfg mapped at phys 0x%lx KVA %p\n",
		    (unsigned long)RP1_ETH_CFG_BASE_PHYS, sc->cfg_kva);

		/* Snapshot firmware-left register state before any writes. */
		printf("rp1_eth: entry snapshot — "
		    "STATUS=0x%08x CLKGEN=0x%08x CTRL=0x%08x INTR=0x%08x "
		    "GEM_NET_CTRL=0x%08x GEM_NET_CFG=0x%08x GEM_USRIO=0x%08x\n",
		    CFG_RD4(sc, ETH_CFG_STATUS),
		    CFG_RD4(sc, ETH_CFG_CLKGEN),
		    CFG_RD4(sc, ETH_CFG_CONTROL),
		    CFG_RD4(sc, ETH_CFG_INTR),
		    MAC_RD4(sc, GEM_NET_CTRL),
		    MAC_RD4(sc, GEM_NET_CFG),
		    MAC_RD4(sc, GEM_USRIO));

		/*
		 * Ensure the RGMII clock generator is running.
		 *
		 * CLKGEN.ENABLE (bit 7) has a reset value of 1, but U-Boot may
		 * have called a clean shutdown of the clock generator (clearing
		 * ENABLE or asserting KILL) when it finished using Ethernet.
		 * If ENABLE=0, the RGMII interface has no clock and eth_cfg.STATUS
		 * will be frozen regardless of cable state.
		 *
		 * Safe sequence per datasheet: clear KILL first, then set ENABLE.
		 */
		{
			uint32_t clkgen = CFG_RD4(sc, ETH_CFG_CLKGEN);
			static const char * const spd_name[] =
			    { "10M", "100M", "1G", "?" };

			printf("rp1_eth: eth_cfg.CLKGEN on entry = 0x%08x "
			    "(ENABLE=%u KILL=%u TXCLKDELEN=%u "
			    "SPD_FROM_MAC=%s SPD_OVR_EN=%u SPD_OVR=%s)\n",
			    clkgen,
			    (clkgen & ETH_CFG_CLKGEN_ENABLE)    ? 1 : 0,
			    (clkgen & ETH_CFG_CLKGEN_KILL)       ? 1 : 0,
			    (clkgen & ETH_CFG_CLKGEN_TXCLKDELEN) ? 1 : 0,
			    spd_name[(clkgen >> 4) & 3],
			    (clkgen & ETH_CFG_CLKGEN_SPD_OVR_EN) ? 1 : 0,
			    spd_name[clkgen & 3]);

			if (!(clkgen & ETH_CFG_CLKGEN_ENABLE) ||
			     (clkgen & ETH_CFG_CLKGEN_KILL)) {
				clkgen &= ~ETH_CFG_CLKGEN_KILL;
				clkgen |=  ETH_CFG_CLKGEN_ENABLE;
				CFG_WR4(sc, ETH_CFG_CLKGEN, clkgen);
				printf("rp1_eth: CLKGEN restarted → 0x%08x\n",
				    CFG_RD4(sc, ETH_CFG_CLKGEN));
			}

			/*
			 * phy-mode=rgmii-id: BCM54213PE applies TX+RX delays
			 * internally.  TXCLKDELEN must be 0 — adding MAC-side
			 * TX delay on top of the PHY-internal delay causes CRC
			 * errors at 1G.  Log a warning but don't fail here;
			 * the SPD_OVR update below will clear TXCLKDELEN.
			 */
			if (clkgen & ETH_CFG_CLKGEN_TXCLKDELEN)
				printf("rp1_eth: WARNING CLKGEN.TXCLKDELEN=1 "
				    "but phy-mode=rgmii-id — will be cleared\n");

			/*
			 * Pre-set CLKGEN to 1G (125 MHz TXCLK) with SPD_OVR
			 * BEFORE asserting the PHY hardware reset.
			 *
			 * BCM54213PE samples the TXCLK frequency during its
			 * power-on reset sequence.  If TXCLK is at 2.5 MHz
			 * (10M default from SPD_FROM_MAC when NET_CFG is at
			 * reset), BCM initialises its RGMII state machine for
			 * 10M even if its copper link subsequently negotiates
			 * 1G.  The RGMII link then never establishes and
			 * eth_cfg.STATUS stays 0.
			 *
			 * By forcing 125 MHz before the hardware reset is
			 * released, BCM sees the correct clock from the first
			 * cycle it runs and initialises RGMII for 1G properly.
			 */
			{
				uint32_t cg_1g;

				cg_1g = (clkgen &
				    ~(ETH_CFG_CLKGEN_KILL |
				      ETH_CFG_CLKGEN_TXCLKDELEN |
				      ETH_CFG_CLKGEN_SPD_OVR_EN |
				      ETH_CFG_CLKGEN_SPD_OVR_MASK)) |
				    ETH_CFG_CLKGEN_SPD_OVR_EN |
				    ETH_CFG_CLKGEN_SPD_1G;

				/* Stop cleanly, apply 1G override, restart */
				CFG_WR4(sc, ETH_CFG_CLKGEN, cg_1g &
				    ~ETH_CFG_CLKGEN_ENABLE);
				DELAY(100);
				CFG_WR4(sc, ETH_CFG_CLKGEN,
				    cg_1g | ETH_CFG_CLKGEN_ENABLE);
				DELAY(1000);  /* 1 ms PLL settle */
				printf("rp1_eth: CLKGEN pre-set to 1G → "
				    "0x%08x (125 MHz TXCLK before PHY reset)\n",
				    CFG_RD4(sc, ETH_CFG_CLKGEN));
			}
		}

		/*
		 * Step 2d: Initialise GEM MAC following Linux macb_init_hw /
		 * cgem_config conventions.
		 *
		 * Key insight: eth_cfg.CLKGEN.SPD_FROM_MAC tracks GEM_NET_CFG
		 * speed bits to generate the correct RGMII TX clock frequency.
		 * If NET_CFG retains firmware's speed setting (e.g. 10M reset
		 * default) but the PHY negotiates 1G, the CLKGEN generates a
		 * 2.5 MHz TX clock instead of 125 MHz.  The PHY detects the
		 * clock mismatch and refuses to establish the RGMII data link,
		 * leaving eth_cfg.STATUS=0 even though PHY BMSR shows link UP.
		 *
		 * Convention (from cgem_config / macb_handle_link_change):
		 *   1. Reset MAC to clear all stale firmware state.
		 *   2. Pre-configure NET_CFG for 1G FD (the most common case).
		 *   3. Enable MDIO only (RXEN deferred until speed is known).
		 *   4. Set USRIO=RGMII while RXEN=0 so the mode is latched
		 *      correctly on the RXEN 0→1 transition.
		 *   5. After autoneg: read GBSR/ANLPAR, resolve actual
		 *      speed/duplex, update NET_CFG, then enable RXEN.
		 *   6. Re-write USRIO after each NET_CTRL write because
		 *      GEM_GXL resets USRIO whenever NET_CTRL is written.
		 */
		{
			uint32_t net_cfg;

			/*
			 * Step 2d-1: Full MAC reset (macb_reset_hw convention).
			 * Clears NET_CTRL, USRIO, and all TX/RX state from any
			 * previous firmware or module load.
			 */
			MAC_WR4(sc, GEM_NET_CTRL, 0);
			printf("rp1_eth: GEM reset — "
			    "STATUS=0x%08x after NET_CTRL=0\n",
			    CFG_RD4(sc, ETH_CFG_STATUS));

			/*
			 * Step 2d-2: Configure NET_CFG.
			 * Start with 1G FD (cgem_config default); will be updated
			 * to actual autoneg result before RXEN is enabled.
			 * MDC_DIV=÷96 for RP1 APB ~200 MHz → MDC ≈ 2.08 MHz.
			 * INBAND_STATUS (bit 27 / "SGMII") must NOT be set for
			 * RGMII — it switches the GEM to SGMII serial mode and
			 * breaks the parallel RGMII connection to BCM54213PE.
			 */
			/*
			 * Default to 1G FD.  For 1G, set GIGE_EN only —
			 * NOT SPEED100.  eth_cfg.CLKGEN.SPD_FROM_MAC encodes
			 * the MAC speed as {GIGE_EN, SPEED100}: 0b10 = 1G.
			 * Setting both bits gives 0b11 (undefined) which puts
			 * CLKGEN in an unknown state and leaves STATUS frozen.
			 */
			net_cfg = GEM_NET_CFG_MDC_DIV96 |
			    GEM_NET_CFG_GIGE_EN |
			    GEM_NET_CFG_FULL_DUPLEX;
			MAC_WR4(sc, GEM_NET_CFG, net_cfg);

			/*
			 * Step 2d-3: Enable MDIO management port only.
			 * RXEN is deferred until after PHY autoneg and NET_CFG
			 * has been updated with the actual negotiated speed.
			 */
			MAC_WR4(sc, GEM_NET_CTRL, GEM_NET_CTRL_MDIO_EN);

			/*
			 * Step 2d-4: Set USRIO=RGMII while RXEN=0.
			 * The GEM_GXL resets USRIO to 0 on any NET_CTRL write.
			 * Writing USRIO here (with RXEN still 0) ensures the
			 * RGMII mode is latched when RXEN is asserted later.
			 * USRIO must be re-written after the RXEN-enabling
			 * NET_CTRL write (step 5f below).
			 */
			MAC_WR4(sc, GEM_USRIO, GEM_USRIO_RGMII);

			printf("rp1_eth: GEM pre-init — "
			    "NET_CTRL=0x%08x NET_CFG=0x%08x USRIO=0x%08x\n",
			    MAC_RD4(sc, GEM_NET_CTRL),
			    MAC_RD4(sc, GEM_NET_CFG),
			    MAC_RD4(sc, GEM_USRIO));
		}

		/*
		 * Step 3: Build sysctl tree hw.rp1_eth.*
		 *
		 * Note: in Milestone 2 this moves under dev.rp1_eth.0.* once
		 * a synthesized device_t is available.
		 */
		tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
		    SYSCTL_STATIC_CHILDREN(_hw),
		    OID_AUTO, "rp1_eth",
		    CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
		    "RP1 Ethernet (Cadence GEM_GXL)");
		sc->sysctl_tree = tree;

		if (tree == NULL) {
			printf("rp1_eth: failed to create sysctl node\n");
			pmap_unmapdev(sc->cfg_kva, RP1_ETH_CFG_MAP_SIZE);
			sysctl_ctx_free(&sc->sysctl_ctx);
			mtx_destroy(&sc->sc_mtx);
			free(sc, M_RP1ETH);
			return (ENOMEM);
		}

		/* hw.rp1_eth.mac_addr — firmware-stamped MAC address */
		mac_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
		    SYSCTL_CHILDREN(tree),
		    OID_AUTO, "mac", CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
		    "FDT-sourced MAC metadata");
		if (mac_tree != NULL) {
			static char mac_str[20];
			snprintf(mac_str, sizeof(mac_str),
			    "%02x:%02x:%02x:%02x:%02x:%02x",
			    sc->mac_addr[0], sc->mac_addr[1],
			    sc->mac_addr[2], sc->mac_addr[3],
			    sc->mac_addr[4], sc->mac_addr[5]);
			SYSCTL_ADD_STRING(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(mac_tree),
			    OID_AUTO, "address",
			    CTLFLAG_RD | CTLFLAG_MPSAFE,
			    mac_str, 0,
			    "Firmware-stamped MAC address (local-mac-address)");
			SYSCTL_ADD_STRING(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(mac_tree),
			    OID_AUTO, "phy_mode",
			    CTLFLAG_RD | CTLFLAG_MPSAFE,
			    sc->phy_mode, 0,
			    "PHY interface mode (phy-mode FDT property)");
			SYSCTL_ADD_UINT(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(mac_tree),
			    OID_AUTO, "phy_mdio_addr",
			    CTLFLAG_RD | CTLFLAG_MPSAFE,
			    &sc->phy_addr, 0,
			    "PHY MDIO address");
		}

		/* hw.rp1_eth.gem.* — GEM core registers (diagnostic read-only) */
		{
			struct sysctl_oid *gem_tree;
			gem_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(tree),
			    OID_AUTO, "gem",
			    CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
			    "GEM MAC core registers (Cadence GEM_GXL)");
#define ADD_MAC_REG(name_, reg_, desc_)					\
	SYSCTL_ADD_PROC(&sc->sysctl_ctx,				\
	    SYSCTL_CHILDREN(gem_tree),					\
	    OID_AUTO, (name_),						\
	    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE,			\
	    sc, (reg_), rp1_eth_mac_reg_sysctl, "IU", (desc_))

			if (gem_tree != NULL) {
				ADD_MAC_REG("net_ctrl",   GEM_NET_CTRL,
				    "NETWORK_CONTROL: RXEN(2) TXEN(3) MDIO_EN(4)");
				ADD_MAC_REG("usrio",      GEM_USRIO,
				    "USER_IO: RGMII(0) — must be 1 for RGMII mode");
				ADD_MAC_REG("net_cfg",    GEM_NET_CFG,
				    "NETWORK_CONFIG: speed/duplex/MDC/INBAND_STATUS(27)");
				ADD_MAC_REG("net_status", GEM_NET_STATUS,
				    "NETWORK_STATUS: MDIO_IDLE(2)");
			}
#undef ADD_MAC_REG
		}

		/* hw.rp1_eth.cfg.* — eth_cfg glue registers */
		cfg_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
		    SYSCTL_CHILDREN(tree),
		    OID_AUTO, "cfg",
		    CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
		    "eth_cfg APB glue registers (RP-008370-DS-1 §7)");
		if (cfg_tree != NULL) {
#define ADD_CFG_REG(name_, reg_, desc_)					\
	SYSCTL_ADD_PROC(&sc->sysctl_ctx,				\
	    SYSCTL_CHILDREN(cfg_tree),					\
	    OID_AUTO, (name_),						\
	    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE,			\
	    sc, (reg_), rp1_eth_cfg_reg_sysctl, "IU", (desc_))

			ADD_CFG_REG("control", ETH_CFG_CONTROL,
			    "CONTROL: bus-error + reset bits");
			ADD_CFG_REG("clkgen",  ETH_CFG_CLKGEN,
			    "CLKGEN: TX/RX delay enables, speed override");
			ADD_CFG_REG("clk2fc",  ETH_CFG_CLK2FC,
			    "CLK2FC: clock-to-FC delay");
			ADD_CFG_REG("intr",    ETH_CFG_INTR,
			    "INTR: raw interrupt status (write-1-clear)");
			ADD_CFG_REG("inte",    ETH_CFG_INTE,
			    "INTE: interrupt enable");
#undef ADD_CFG_REG

			/*
			 * status has both a raw hex sysctl and a formatted
			 * string sysctl for quick human-readable observation.
			 */
			SYSCTL_ADD_PROC(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(cfg_tree),
			    OID_AUTO, "status",
			    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE,
			    sc, ETH_CFG_STATUS,
			    rp1_eth_cfg_reg_sysctl, "IU",
			    "STATUS: raw register value");
			SYSCTL_ADD_PROC(&sc->sysctl_ctx,
			    SYSCTL_CHILDREN(cfg_tree),
			    OID_AUTO, "status_decoded",
			    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
			    sc, 0,
			    rp1_eth_cfg_status_fmt_sysctl, "A",
			    "STATUS decoded: link/speed/duplex/errors");
		}

		/*
		 * Step 4: PHY reset / deassert.
		 *
		 * The firmware leaves GPIO32 configured as a pure INPUT
		 * (FUNCSEL=31/NULL, OUTOVER=0, OEOVER=0) after using Ethernet
		 * during boot.  The RP1 GPIO has a pull-DOWN on this pin by
		 * default, which pulls RESET_N LOW → BCM54213PE held in hardware
		 * reset at module load time.  This explains:
		 *   - MDIO returning 0xffff (PHY in reset ignores management frames)
		 *   - eth_cfg.STATUS = 0 (PHY not driving RGMII signals)
		 *
		 * We must drive GPIO32 HIGH (deassert RESET_N) via the correct
		 * RP1 CTRL bits (OUTOVER at [13:12], OEOVER at [15:14]).
		 * Then wait for BCM PHY power-up + auto-negotiation (~3 s).
		 */
		rp1_eth_phy_reset(sc);

		/*
		 * Early RXEN+TXEN: enable MAC receive/transmit immediately
		 * after PHY deassert so that eth_cfg.STATUS is live throughout
		 * the autoneg window.  NET_CFG is already set to 1G FD and
		 * CLKGEN is pre-set to 125 MHz, so the MAC can sample RGMII
		 * in-band status from the first BCM cycle after power-up.
		 *
		 * GEM_GXL resets USRIO on any NET_CTRL write — always follow
		 * a NET_CTRL write with USRIO=RGMII.
		 *
		 * TXEN is required: BCM54213PE only asserts RGMII in-band
		 * LINK=1 on RXD[3:0] when it detects TX_CTL active from
		 * the MAC side.  Without TXEN, BCM sends LINK=0 even when
		 * the copper link is UP.
		 */
		CFG_WR4(sc, ETH_CFG_INTR, CFG_RD4(sc, ETH_CFG_INTR));
		MAC_WR4(sc, GEM_NET_CTRL,
		    GEM_NET_CTRL_MDIO_EN | GEM_NET_CTRL_RXEN | GEM_NET_CTRL_TXEN);
		MAC_WR4(sc, GEM_USRIO, GEM_USRIO_RGMII);
		printf("rp1_eth: early RXEN+TXEN "
		    "(NET_CTRL=0x%08x USRIO=0x%08x STATUS=0x%08x INTR=0x%08x)\n",
		    MAC_RD4(sc, GEM_NET_CTRL),
		    MAC_RD4(sc, GEM_USRIO),
		    CFG_RD4(sc, ETH_CFG_STATUS),
		    CFG_RD4(sc, ETH_CFG_INTR));

		/*
		 * Step 5: MDIO diagnostic — read PHY BMCR/BMSR/ID to verify
		 * the BCM PHY is alive and report its link state independently
		 * of eth_cfg.STATUS.  This disambiguates "PHY has no link" from
		 * "eth_cfg STATUS not working".
		 *
		 * After autoneg: resolve actual speed/duplex from GBSR/ANLPAR
		 * and update GEM_NET_CFG so that eth_cfg.CLKGEN generates the
		 * correct RGMII TX clock frequency.
		 */
		{
			uint16_t bmcr, bmsr, phyid1, phyid2;
			uint16_t gbsr, anlpar, gbcr;
			uint32_t net_cfg;
			uint32_t st_iter, intr_iter;
			int speed, full_duplex;

			phyid1 = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_PHYIDR1);
			phyid2 = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_PHYIDR2);
			bmcr   = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_BMCR);
			bmsr   = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_BMSR);

			/* Second BMSR read required for latching link-down events */
			bmsr   = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_BMSR);

			printf("rp1_eth: PHY@%u ID 0x%04x:0x%04x  "
			    "BMCR=0x%04x BMSR=0x%04x "
			    "(link=%s an=%s)\n",
			    sc->phy_addr, phyid1, phyid2, bmcr, bmsr,
			    (bmsr & BMSR_LINK)    ? "UP"  : "DOWN",
			    (bmsr & BMSR_AN_DONE) ? "done" : "in-progress");

			/*
			 * Apply BCM54213PE RGMII-ID delay config BEFORE autoneg.
			 *
			 * Linux calls bcm54xx_config_clock_delay() +
			 * bcm54xx_adjust_rxrefclk() in config_init, before
			 * config_aneg (autoneg start).  Doing it after autoneg
			 * is too late — the PHY's RGMII state machine has
			 * already committed to wrong delay settings.
			 *
			 * Three settings for rgmii-id (both TX and RX delays
			 * in PHY):
			 *
			 *  1. AuxCtl MISC RGMII_SKEW_EN (bit 8): RX clock-data
			 *     skew.  Read protocol: write 0x7007 (not 0x0007!)
			 *     so bits[14:12]=7 selects MISC shadow for reading.
			 *     Write protocol: 0x0007 | WREN(bit15) | val.
			 *
			 *  2. BCM54810_SHD_CLK_CTL GTXCLK_EN (bit 9): internal
			 *     TX clock delay.
			 *     Write: 0x8000 | (shadow3<<10) | val.
			 *
			 *  3. SCR3 DLLAPD_DIS (bit 1 = 0x0002) = 1: power down
			 *     the DLL (bcm54xx_adjust_rxrefclk sets this for
			 *     BCM54213PE; RGMII_SKEW_EN replaces DLL for RX).
			 *     Write: 0x8000 | (shadow5<<10) | val.
			 */
			{
				uint16_t auxmisc_r, clkctl_r, scr3_r;

				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x18, (7u << 12) | 0x0007u);
				auxmisc_r = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x18);
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x0003u << 10);
				clkctl_r  = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x0005u << 10);
				scr3_r    = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;

				/* 1. AuxCtl MISC: set RGMII_SKEW_EN + WREN */
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x18, 0x0007u |
				    (auxmisc_r | 0x0100u | 0x8000u));

				/* 2. CLKCTL (shadow 3): set GTXCLK_EN */
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x8000u | (0x0003u << 10) |
				    ((clkctl_r | (1u << 9)) & 0x03ffu));

				/* 3. SCR3 (shadow 5): set DLLAPD_DIS */
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x8000u | (0x0005u << 10) |
				    ((scr3_r | 0x0002u) & 0x03ffu));

				printf("rp1_eth: BCM config_init "
				    "(before autoneg) — "
				    "AuxMISC pre=0x%04x CLKCTL pre=0x%03x "
				    "SCR3 pre=0x%03x STATUS=0x%08x\n",
				    auxmisc_r, clkctl_r, scr3_r,
				    CFG_RD4(sc, ETH_CFG_STATUS));
			}

			/*
			 * Wait for auto-negotiation to complete (up to 4 s).
			 * BCM PHY needs ~150 ms power-up (already waited above)
			 * plus ~1-3 s for 1G auto-neg with the link partner.
			 */
			if (!(bmsr & BMSR_LINK)) {
				int t;
				printf("rp1_eth: waiting up to 4 s for "
				    "auto-negotiation...\n");
				for (t = 0; t < 40; t++) {
					pause("rp1_eth_an", hz / 10);
					bmsr = rp1_eth_mdio_read(sc,
					    (int)sc->phy_addr, MII_BMSR);
					bmsr = rp1_eth_mdio_read(sc,
					    (int)sc->phy_addr, MII_BMSR);
					st_iter   = CFG_RD4(sc, ETH_CFG_STATUS);
					intr_iter = CFG_RD4(sc, ETH_CFG_INTR);
					printf("rp1_eth: an t=%d00ms "
					    "BMSR=0x%04x(lnk=%s an=%s) "
					    "STATUS=0x%08x INTR=0x%08x\n",
					    t + 1, bmsr,
					    (bmsr & BMSR_LINK)    ? "UP" : "dn",
					    (bmsr & BMSR_AN_DONE) ? "ok" : "wait",
					    st_iter, intr_iter);
					if (bmsr & BMSR_LINK)
						break;
				}
				printf("rp1_eth: auto-neg %s after %d00 ms "
				    "(BMSR=0x%04x)\n",
				    (bmsr & BMSR_LINK) ? "complete" : "timed out",
				    t + 1, bmsr);
			}

			/*
			 * Step 5d: BCM54213PE diagnostic register dump.
			 *
			 * Log vendor-specific registers to diagnose why
			 * eth_cfg.STATUS does not update:
			 *
			 *  reg 0x10 (ECR) — Extended Control Register:
			 *    bit 8 = INBAND_STATUS_DISABLE (if 1: PHY does not
			 *            drive RGMII in-band status → STATUS stays 0)
			 *
			 *  AuxCtl shadow 7 (reg 0x18 after writing 0x0007):
			 *    bit 9 = GTXCLKOUT_EN (TX clock delay)
			 *    bit 1 = RGMII_SKEW_EN (additional RX skew)
			 *
			 *  Shadow reg 5 / SCR3 (reg 0x1c after writing 0x1400):
			 *    bit 1 = DLLAPD_DIS (0=DLL on=RX delay active)
			 *
			 *  Shadow reg 0x1c (Misc Status / Mode Select):
			 *    May reveal strap pin values and interface mode.
			 */
			{
				uint16_t ecr, auxmisc, clkctl, scr3, shd1c;

				/*
				 * Read BCM54213PE diagnostic registers.
				 *
				 * AuxCtl MISC (reg 0x18, shadow 7):
				 *   bit 15 = WREN (must be set on writes)
				 *   bit  8 = RGMII_SKEW_EN (RX clock-data skew)
				 *   bit  7 = RGMII_EN
				 *   read by writing shadow-select 0x0007 then reading
				 *
				 * BCM54810_SHD_CLK_CTL (reg 0x1c, shadow 3):
				 *   bit  9 = GTXCLK_EN (TX internal clock delay)
				 *   read by writing 3<<10 = 0x0c00 then reading
				 *
				 * SCR3 (reg 0x1c, shadow 5):
				 *   bit  1 = DLLAPD_DIS (1=DLL off = no RX delay)
				 *   read by writing 5<<10 = 0x1400 then reading
				 *
				 * Sources: brcmphy.h, broadcom.c bcm54xx_config_clock_delay()
				 */
				ecr = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x10);
				/* AuxCtl MISC read: write selects BOTH
				 * write-shadow (bits[2:0]=7) and read-shadow
				 * (bits[14:12]=7) — Linux uses 0x7007 */
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x18, (7u << 12) | 0x0007u);
				auxmisc = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x18);
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x0003u << 10);
				clkctl = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x0005u << 10);
				scr3 = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x001cu << 10);
				shd1c = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;

				printf("rp1_eth: BCM diag — "
				    "ECR=0x%04x(INBAND_DIS=%u) "
				    "AuxMISC=0x%04x(RGMII_EN=%u SKEW_EN=%u) "
				    "CLKCTL=0x%03x(GTXCLK_EN=%u) "
				    "SCR3=0x%03x(DLLAPD_DIS=%u) "
				    "SHD1c=0x%03x\n",
				    ecr,   (ecr >> 8) & 1,
				    auxmisc,
				    (auxmisc >> 7) & 1,
				    (auxmisc >> 8) & 1,
				    clkctl, (clkctl >> 9) & 1,
				    scr3,  (scr3 >> 1) & 1,
				    shd1c);
				printf("rp1_eth: GEM NET_STATUS=0x%08x\n",
				    MAC_RD4(sc, GEM_NET_STATUS));
			}

			/*
			 * Step 5d2: Verify BCM clock-delay settings post-autoneg.
			 * Writes were already applied in the pre-autoneg block
			 * (matching Linux config_init timing).  This is read-only.
			 */
			{
				uint16_t auxmisc_v, clkctl_v, scr3_v;

				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x18, (7u << 12) | 0x0007u);
				auxmisc_v = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x18);
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x0003u << 10);
				clkctl_v  = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;
				rp1_eth_mdio_write(sc, (int)sc->phy_addr,
				    0x1c, 0x0005u << 10);
				scr3_v    = rp1_eth_mdio_read(sc,
				    (int)sc->phy_addr, 0x1c) & 0x03ffu;

				printf("rp1_eth: BCM clk-delay verify "
				    "(post-autoneg) — "
				    "AuxMISC=0x%04x(SKEW_EN=%u) "
				    "CLKCTL=0x%03x(GTXCLK_EN=%u) "
				    "SCR3=0x%03x(DLLAPD_DIS=%u) "
				    "STATUS=0x%08x\n",
				    auxmisc_v, (auxmisc_v >> 8) & 1,
				    clkctl_v,  (clkctl_v  >> 9) & 1,
				    scr3_v,    (scr3_v    >> 1) & 1,
				    CFG_RD4(sc, ETH_CFG_STATUS));
			}

			/*
			 * Step 5e: Resolve negotiated speed/duplex.
			 *
			 * Read GBSR (reg 10, IEEE 1000Base-T Status) for 1G
			 * capability and ANLPAR (reg 5, AN Link Partner Ability)
			 * for 100/10 Mbps.  Also log our own advertisement (GBCR
			 * reg 9) to confirm we offered 1G.
			 *
			 * Resolution priority (IEEE 802.3 clause 28): 1G FD >
			 * 1G HD > 100 FD > 100 HD > 10 FD > 10 HD.
			 */
			gbcr   = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_GBCR);
			gbsr   = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_GBSR);
			anlpar = rp1_eth_mdio_read(sc, (int)sc->phy_addr, MII_ANLPAR);

			printf("rp1_eth: PHY@%u GBCR=0x%04x GBSR=0x%04x "
			    "ANLPAR=0x%04x\n",
			    sc->phy_addr, gbcr, gbsr, anlpar);

			if ((gbcr & GBCR_ADV_1000FD) &&
			    (gbsr & GBSR_LP_1000FD)) {
				speed = 1000; full_duplex = 1;
			} else if (gbsr & GBSR_LP_1000HD) {
				speed = 1000; full_duplex = 0;
			} else if (anlpar & ANLPAR_100FD) {
				speed = 100;  full_duplex = 1;
			} else if (anlpar & ANLPAR_100HD) {
				speed = 100;  full_duplex = 0;
			} else if (anlpar & ANLPAR_10FD) {
				speed = 10;   full_duplex = 1;
			} else {
				speed = 10;   full_duplex = 0;
			}

			printf("rp1_eth: resolved %dM %s-duplex%s\n",
			    speed, full_duplex ? "full" : "half",
			    (bmsr & BMSR_LINK) ? "" : " (no link — defaults)");

			/*
			 * Step 5f: Update GEM_NET_CFG with the resolved speed.
			 *
			 * Use macb_handle_link_change() convention:
			 *   1G:   GIGE_EN only — NOT SPEED100
			 *   100M: SPEED100 only — NOT GIGE_EN
			 *   10M:  neither
			 * SPEED100 must NOT be set for 1G: eth_cfg.CLKGEN encodes
			 * speed as {GIGE_EN,SPEED100}; setting both gives 0b11
			 * (undefined) which confuses the RGMII clock/status logic.
			 */
			net_cfg = MAC_RD4(sc, GEM_NET_CFG);
			net_cfg &= ~GEM_NET_CFG_SPEED_MASK;
			if (speed == 1000)
				net_cfg |= GEM_NET_CFG_GIGE_EN;
			else if (speed == 100)
				net_cfg |= GEM_NET_CFG_SPEED100;
			if (full_duplex)
				net_cfg |= GEM_NET_CFG_FULL_DUPLEX;
			MAC_WR4(sc, GEM_NET_CFG, net_cfg);
			printf("rp1_eth: NET_CFG updated → 0x%08x "
			    "(SPD_FROM_MAC=0x%x)\n",
			    net_cfg,
			    (CFG_RD4(sc, ETH_CFG_CLKGEN) >> 4) & 3);

			/*
			 * Step 5g: Program CLKGEN speed override.
			 *
			 * BCM54213PE validates the incoming RGMII TXCLK
			 * frequency against its negotiated copper link speed.
			 * If TXCLK is wrong (e.g. 2.5 MHz from the default 10M
			 * state while copper is at 1G), BCM won't drive RGMII
			 * signals and eth_cfg.STATUS stays 0x00000000 even
			 * though PHY BMSR shows link UP.
			 *
			 * SPD_FROM_MAC (RO field in CLKGEN) may not update until
			 * RXEN is active and the GEM is fully running — a
			 * chicken-and-egg that we break by using SPD_OVR_EN to
			 * directly program the CLKGEN frequency.
			 *
			 * Safe sequence: clear ENABLE (stop cleanly) → write
			 * new SPD_OVR → assert ENABLE (restart at new rate).
			 */
			{
				uint32_t spd_val, clkgen, clkgen_base;

				spd_val = (speed == 1000) ?
				    ETH_CFG_CLKGEN_SPD_1G :
				    (speed == 100) ?
				    ETH_CFG_CLKGEN_SPD_100M :
				    ETH_CFG_CLKGEN_SPD_10M;

				clkgen = CFG_RD4(sc, ETH_CFG_CLKGEN);
				printf("rp1_eth: CLKGEN before speed "
				    "update = 0x%08x "
				    "(ENABLE=%u KILL=%u SPD_FROM_MAC=%u)\n",
				    clkgen,
				    (clkgen & ETH_CFG_CLKGEN_ENABLE) ? 1 : 0,
				    (clkgen & ETH_CFG_CLKGEN_KILL)   ? 1 : 0,
				    (clkgen >> 4) & 3);

				/* Base: keep reserved bits, clear speed + TXCLKDEL */
				clkgen_base = clkgen &
				    ~(ETH_CFG_CLKGEN_ENABLE |
				      ETH_CFG_CLKGEN_KILL |
				      ETH_CFG_CLKGEN_TXCLKDELEN |
				      ETH_CFG_CLKGEN_SPD_OVR_EN |
				      ETH_CFG_CLKGEN_SPD_OVR_MASK);
				clkgen_base |= ETH_CFG_CLKGEN_SPD_OVR_EN |
				    spd_val;

				/* Stop cleanly (clear ENABLE, no KILL glitch) */
				CFG_WR4(sc, ETH_CFG_CLKGEN, clkgen_base);
				DELAY(100);

				/* Restart at new speed */
				CFG_WR4(sc, ETH_CFG_CLKGEN,
				    clkgen_base | ETH_CFG_CLKGEN_ENABLE);
				DELAY(10000);	/* 10 ms: PLL settle */

				printf("rp1_eth: CLKGEN → 0x%08x "
				    "(SPD_OVR=%uM, %u MHz TXCLK)\n",
				    CFG_RD4(sc, ETH_CFG_CLKGEN),
				    speed,
				    speed == 1000 ? 125 :
				    speed == 100  ? 25  : 2);
			}

			/*
			 * Step 5h: Re-assert RXEN+TXEN after NET_CFG/CLKGEN update.
			 *
			 * Writing GEM_NET_CFG (step 5f) does NOT clear NET_CTRL,
			 * but writing ETH_CFG_CLKGEN (step 5g) cycles the clock
			 * generator.  Re-assert RXEN+TXEN here to ensure the MAC
			 * is active at the final negotiated speed/clock settings.
			 *
			 * GEM_GXL resets USRIO on every NET_CTRL write — always
			 * follow with USRIO=RGMII.
			 *
			 * Poll STATUS+INTR every 100ms for up to 2s and log each
			 * interval so we can see the exact moment (if any) that
			 * STATUS transitions from 0 to non-zero.
			 */
			CFG_WR4(sc, ETH_CFG_INTR, CFG_RD4(sc, ETH_CFG_INTR));
			MAC_WR4(sc, GEM_NET_CTRL,
			    GEM_NET_CTRL_MDIO_EN |
			    GEM_NET_CTRL_RXEN |
			    GEM_NET_CTRL_TXEN);
			MAC_WR4(sc, GEM_USRIO, GEM_USRIO_RGMII);
			printf("rp1_eth: step5h RXEN+TXEN re-assert "
			    "(NET_CTRL=0x%08x USRIO=0x%08x)\n",
			    MAC_RD4(sc, GEM_NET_CTRL),
			    MAC_RD4(sc, GEM_USRIO));

			{
				uint32_t st;
				int t;

				for (t = 0; t < 20; t++) {
					pause("rp1_eth_st", hz / 10);
					st        = CFG_RD4(sc, ETH_CFG_STATUS);
					intr_iter = CFG_RD4(sc, ETH_CFG_INTR);
					printf("rp1_eth: 5h t=%d00ms "
					    "STATUS=0x%08x INTR=0x%08x\n",
					    t + 1, st, intr_iter);
					if (st != 0)
						break;
				}
			}

			/*
			 * PAUSE frame experiment: assert TX_CTL=1 without DMA
			 * to test whether the BCM54213PE requires a real MAC TX
			 * frame before it will assert LINK=1 in RGMII in-band
			 * status.  NET_CTRL bit 11 = tx_pause_frame: sends one
			 * 802.3x PAUSE frame then self-clears.
			 */
			MAC_WR4(sc, GEM_NET_CTRL,
			    GEM_NET_CTRL_MDIO_EN |
			    GEM_NET_CTRL_RXEN |
			    GEM_NET_CTRL_TXEN |
			    (1u << 11));
			DELAY(10000);	/* 10 ms — one PAUSE frame at 1G */
			printf("rp1_eth: PAUSE frame sent — "
			    "STATUS=0x%08x INTR=0x%08x NET_CTRL=0x%08x\n",
			    CFG_RD4(sc, ETH_CFG_STATUS),
			    CFG_RD4(sc, ETH_CFG_INTR),
			    MAC_RD4(sc, GEM_NET_CTRL));
		}

		/* Store global reference before M2 attach. */
		rp1_eth_sc = sc;

		printf("rp1_eth: Milestone 1 ready — "
		    "check hw.rp1_eth.cfg.status_decoded\n");
		printf("rp1_eth: eth_cfg.STATUS = 0x%08x\n",
		    CFG_RD4(sc, ETH_CFG_STATUS));

		/* Milestone 2: attach Cadence GEM to the network stack. */
		if (rp1eth_attach(sc) != 0)
			printf("rp1_eth: Milestone 2 attach failed "
			    "(M1 diagnostics still available)\n");
		break;

	case MOD_UNLOAD:
		sc = rp1_eth_sc;
		if (sc == NULL)
			break;

		/* Milestone 2: detach network interface first. */
		rp1eth_detach();

		rp1_eth_sc = NULL;

		sysctl_ctx_free(&sc->sysctl_ctx);

		if (sc->cfg_mapped) {
			pmap_unmapdev(sc->cfg_kva, RP1_ETH_CFG_MAP_SIZE);
			sc->cfg_kva = NULL;
			sc->cfg_mapped = 0;
		}

		if (sc->mac_mapped) {
			/* Full MAC reset on unload (macb_reset_hw convention) */
			MAC_WR4(sc, GEM_NET_CTRL, 0);
			pmap_unmapdev(sc->mac_kva, RP1_ETH_MAC_MAP_SIZE);
			sc->mac_kva = NULL;
			sc->mac_mapped = 0;
		}

		mtx_destroy(&sc->sc_mtx);
		free(sc, M_RP1ETH);
		printf("rp1_eth: unloaded\n");
		break;

	default:
		return (EOPNOTSUPP);
	}

	return (0);
}

static moduledata_t rp1_eth_mdata = {
	"rp1_eth",
	rp1_eth_modevent,
	NULL
};

DECLARE_MODULE(rp1_eth, rp1_eth_mdata, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(rp1_eth, 1);
MODULE_DEPEND(rp1_eth, bcm2712_pcie, 1, 1, 1);
