/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 FreeBSD Contributors
 * All rights reserved.
 *
 * RP1 Ethernet (Cadence GEM_GXL 1p09) - hardware constants and softc
 *
 * Address derivation (same pattern as RP1_PWM1_BASE_PHYS in bcm2712_var.h):
 *   pcie2 outbound: CPU phys 0x1f_00000000 → PCIe addr 0
 *   RP1 ranges:     RP1-child 0xc0_40000000 → PCIe addr 0
 *   CPU phys = 0x1f_00000000 + (RP1_child_addr - 0xc0_40000000)
 *
 *   ethernet@100000  FDT child addr 0xc0_40100000 → CPU phys 0x1f_00100000
 *   eth_cfg@104000   FDT child addr 0xc0_40104000 → CPU phys 0x1f_00104000
 *
 * NOTE: The RP1 datasheet (RP-008370-DS-1 §7) lists internal APB addresses
 * as eth@0x40100000 and eth_cfg@0x40104000.  Those include the RP1-internal
 * APB base (0x40000000).  The pcie2 outbound window uses only the offset from
 * the RP1-child base (0xc040000000), which is 0x100000 / 0x104000.
 *
 * References:
 *   RP-008370-DS-1 ch.7 — Cadence GEM_GXL 1p09 registers
 *   bcm2712_var.h        — pcie2 address derivation precedent
 *   if_gem-PLAN.md       — three-milestone design plan
 */

#ifndef _RP1_ETH_VAR_H_
#define _RP1_ETH_VAR_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <machine/bus.h>

/* -----------------------------------------------------------------------
 * Physical addresses (CPU-side, via pcie2 outbound window)
 * ----------------------------------------------------------------------- */

/* Cadence GEM_GXL MAC register window: 16 KiB */
#define RP1_ETH_MAC_BASE_PHYS		0x1f00100000UL
#define RP1_ETH_MAC_MAP_SIZE		0x4000

/* eth_cfg APB glue register window: 4 KiB */
#define RP1_ETH_CFG_BASE_PHYS		0x1f00104000UL
#define RP1_ETH_CFG_MAP_SIZE		0x1000

/*
 * RP1 PADS_BANK1 — pad control for IO_BANK1 (GPIO 28-33).
 * RP1 APB address 0x400f4000 → CPU phys 0x1f000f4000.
 * Layout: offset 0x000 = VOLTAGE_SELECT, offset 0x004+N*4 = pad N control.
 * GPIO32 (IO_BANK1 index 4): offset (4+1)*4 = 0x14.
 *
 * PADS register bits (RP2040/RP1 family):
 *   bit 7 = OD  (open-drain enable: 0=push-pull, 1=open-drain)
 *   bits[6:5] = DRIVE (0=2mA, 1=4mA, 2=8mA, 3=12mA)
 *   bit 4 = PUE (pull-up enable)
 *   bit 3 = PDE (pull-down enable)
 *   bit 2 = SCHMITT
 *   bit 1 = SLEWFAST
 *   bit 0 = IE  (input enable)
 *
 * Firmware value for GPIO32: 0xd6 = OD=1, DRIVE=8mA, PUE=1, PDE=0, IE=0.
 * To override BCM54213PE's open-drain RESET_N assertion we must clear OD (→
 * push-pull HIGH drive).  We also set IE=1 so INFROMPAD reflects the real
 * pad voltage.  The 8mA push-pull drive wins over BCM's open-drain assertion.
 */
#define RP1_PADS_BANK1_BASE_PHYS	0x1f000f4000UL
#define RP1_PADS_MAP_SIZE		0x1000
#define RP1_GPIO32_PAD_OFFSET		((4+1)*4)	/* = 0x14 */

#define RP1_PAD_OD		(1u << 7)	/* open-drain */
#define RP1_PAD_PUE		(1u << 4)	/* pull-up enable */
#define RP1_PAD_PDE		(1u << 3)	/* pull-down enable */
#define RP1_PAD_IE		(1u << 0)	/* input enable */
#define RP1_PAD_PUSHPULL_HIGH_IE  0x57u /* OD=0,DRIVE=8mA,PUE=1,PDE=0,SCHMITT,SLEWFAST,IE=1 */
#define RP1_PAD_FW_DEFAULT	  0xd6u /* OD=1,DRIVE=8mA,PUE=1,PDE=0,SCHMITT,SLEWFAST,IE=0 */

/* -----------------------------------------------------------------------
 * Cadence GEM_GXL core register offsets (MAC APB window at RP1_ETH_MAC_BASE)
 * Source: Cadence GEM_GXL 1p09 User Guide; cross-referenced with if_cgem.c.
 *
 * Milestone 1 follows the Linux macb_init_hw / cgem_config conventions:
 *  1. Reset MAC (NET_CTRL=0)
 *  2. Configure NET_CFG with 1G FD default + MDC_DIV96
 *  3. Enable MDIO (NET_CTRL = MDIO_EN only — RXEN deferred)
 *  4. Set USRIO = RGMII while RXEN=0 so the RGMII mode is latched correctly
 *  5. After PHY autoneg: resolve speed/duplex via MDIO, update NET_CFG
 *  6. Enable RXEN (re-write USRIO after since GEM_GXL resets it on NET_CTRL writes)
 *
 * eth_cfg.CLKGEN.SPD_FROM_MAC tracks NET_CFG speed bits to generate the
 * correct RGMII TX clock.  Speed mismatch (e.g. NET_CFG=10M, PHY=1G) causes
 * the PHY to not establish the RGMII data link even when MDIO shows link UP.
 * ----------------------------------------------------------------------- */
#define GEM_NET_CTRL		0x000	/* network_control */
#define GEM_NET_CFG		0x004	/* network_config */
#define GEM_NET_STATUS		0x008	/* network_status (RO) */
#define GEM_USRIO		0x000c	/* user I/O register */
#define GEM_PHY_MAINT		0x034	/* PHY maintenance (MDIO frame) */

/* GEM_NET_CTRL bits */
#define GEM_NET_CTRL_RXEN	(1u << 2)  /* enable receive RGMII state machine */
#define GEM_NET_CTRL_TXEN	(1u << 3)  /* enable transmit (not needed for M1) */
#define GEM_NET_CTRL_MDIO_EN	(1u << 4)  /* enable MDIO management port */

/*
 * GEM_USRIO — user I/O register (offset 0x000c).
 * Must be written before MDIO and before bringing up RGMII.
 * Linux macb driver writes GEM_BIT(RGMII) = bit 0 for any RGMII variant.
 * Without this the GEM assumes GMII mode and cannot decode RGMII signals,
 * which is why eth_cfg.STATUS never updates even with RXEN=1.
 */
#define GEM_USRIO_RGMII		(1u << 0)  /* select RGMII external interface */

/* GEM_NET_STATUS bits */
#define GEM_NET_STATUS_MDIO_IDLE (1u << 2)  /* MDIO bus idle (management frame done) */

/*
 * GEM_PHY_MAINT — Clause-22 MDIO frame register.
 * Write a complete frame; read back data[15:0] when MDIO_IDLE.
 */
#define GEM_MAINT_SOF		(0x1u << 30)  /* start of frame */
#define GEM_MAINT_OP_RD		(0x2u << 28)  /* read operation */
#define GEM_MAINT_OP_WR		(0x1u << 28)  /* write operation */
#define GEM_MAINT_PHY(x)	(((x) & 0x1f) << 23)
#define GEM_MAINT_REG(x)	(((x) & 0x1f) << 18)
#define GEM_MAINT_TA		(0x2u << 16)  /* turnaround (must be 10) */
#define GEM_MAINT_DATA(x)	((x) & 0xffff)

/* Standard IEEE 802.3 PHY register numbers */
#define MII_BMCR	0   /* Basic Mode Control */
#define  BMCR_RESET	(1u << 15)  /* software reset (self-clearing) */
#define MII_BMSR	1   /* Basic Mode Status */
#define  BMSR_LINK	(1u << 2)   /* link status */
#define  BMSR_AN_DONE	(1u << 5)   /* auto-negotiation complete */
#define MII_PHYIDR1	2   /* PHY ID word 1 */
#define MII_PHYIDR2	3   /* PHY ID word 2 */
#define MII_ANLPAR	5   /* Auto-Negotiation Link Partner Ability */
#define  ANLPAR_100FD	(1u << 8)  /* LP 100Base-TX full-duplex */
#define  ANLPAR_100HD	(1u << 7)  /* LP 100Base-TX half-duplex */
#define  ANLPAR_10FD	(1u << 6)  /* LP 10Base-T  full-duplex */
#define  ANLPAR_10HD	(1u << 5)  /* LP 10Base-T  half-duplex */
#define MII_GBCR	9   /* 1000Base-T Control (our advertisements) */
#define  GBCR_ADV_1000FD (1u << 9) /* Advertise 1000Base-T full-duplex */
#define MII_GBSR	10  /* 1000Base-T Status (link partner abilities) */
#define  GBSR_LP_1000FD	(1u << 11) /* LP 1000Base-T full-duplex capable */
#define  GBSR_LP_1000HD	(1u << 10) /* LP 1000Base-T half-duplex capable */

/*
 * GEM_NET_CFG important bits.
 *
 * Speed/duplex bits — must match the negotiated PHY speed so that
 * eth_cfg.CLKGEN generates the correct RGMII TX clock (SPD_FROM_MAC
 * tracks these bits).  If NET_CFG says 10M but the PHY negotiated 1G,
 * the 2.5 MHz TX clock causes the PHY to not establish the RGMII data
 * link even after autoneg completes, explaining BMSR="link UP" while
 * eth_cfg.STATUS stays 0.
 *
 * SPEED/GIGE_EN encoding (from macb_handle_link_change, Cadence GEM_GXL):
 *   1G full:   GEM_NET_CFG_GIGE_EN | GEM_NET_CFG_FULL_DUPLEX
 *              (SPEED100 must NOT be set; eth_cfg.CLKGEN.SPD_FROM_MAC encodes
 *               speed as {GIGE_EN, SPEED100} — setting both gives 0b11 = undefined,
 *               only GIGE_EN gives 0b10 = 1G which CLKGEN uses to generate 125 MHz)
 *   100M full: GEM_NET_CFG_SPEED100 | GEM_NET_CFG_FULL_DUPLEX
 *   10M full:  GEM_NET_CFG_FULL_DUPLEX only
 *
 * INBAND_STATUS (bit 27, labelled "SGMII" in the Cadence User Guide):
 *   On RP1 GEM_GXL this switches the external interface to SGMII serial mode,
 *   breaking the parallel RGMII connection to BCM54213PE.  Linux macb driver
 *   does NOT set this bit for RGMII mode — it writes GEM_USRIO[0] instead.
 *
 * MDC clock divisor field (bits 20:18):
 *   Linux macb driver selects DIV based on pclk frequency.  The RP1 GEM pclk
 *   appears to be ~200 MHz (firmware chose MDC_DIV=5 = ÷96 → MDC ≈ 2.08 MHz).
 */
#define GEM_NET_CFG_GIGE_EN		(1u << 10) /* gigabit mode (GEM extension) */
#define GEM_NET_CFG_FULL_DUPLEX		(1u << 1)  /* full-duplex */
#define GEM_NET_CFG_SPEED100		(1u << 0)  /* 100 Mbps (also set for 1G) */
/* Mask covering all speed/duplex bits (for clear-before-set pattern) */
#define GEM_NET_CFG_SPEED_MASK		(GEM_NET_CFG_GIGE_EN | \
					 GEM_NET_CFG_FULL_DUPLEX | \
					 GEM_NET_CFG_SPEED100)
#define GEM_NET_CFG_INBAND_STATUS	(1u << 27) /* SGMII mode — breaks RGMII on RP1 */
#define GEM_NET_CFG_MDC_DIV_SHIFT	18
#define GEM_NET_CFG_MDC_DIV_MASK	(7u << 18)
/* MDC divisors: 0→÷8, 1→÷16, 2→÷32, 3→÷48, 4→÷64, 5→÷96, 6→÷128, 7→÷224 */
#define GEM_NET_CFG_MDC_DIV96		(5u << 18)  /* pclk/96 — Pi 5 RP1 APB ~200 MHz */

/* GEM_NET_STATUS bits (RO) */
#define GEM_NET_STATUS_LINK		(1u << 0)  /* PCS link — not RGMII pin */
#define GEM_NET_STATUS_MDIO_IDLE	(1u << 2)  /* MDIO bus idle */

/* MAC register accessors (no lock needed in M1 single-threaded init) */
#define MAC_RD4(sc, reg) \
	(*(volatile uint32_t *)((uintptr_t)(sc)->mac_kva + (reg)))
#define MAC_WR4(sc, reg, val) \
	(*(volatile uint32_t *)((uintptr_t)(sc)->mac_kva + (reg)) = (val))

/* eth_cfg register write accessor */
#define CFG_WR4(sc, reg, val) \
	(*(volatile uint32_t *)((uintptr_t)(sc)->cfg_kva + (reg)) = (val))

/* -----------------------------------------------------------------------
 * eth_cfg register offsets (RP-008370-DS-1 §7, APB atomic window)
 * ----------------------------------------------------------------------- */
#define ETH_CFG_CONTROL		0x00	/* bus-error, reset control */
#define ETH_CFG_STATUS		0x04	/* RGMII link state (read-only) */
#define ETH_CFG_TSU_CNT0	0x08	/* TSU timer count word 0 */
#define ETH_CFG_TSU_CNT1	0x0c	/* TSU timer count word 1 */
#define ETH_CFG_TSU_CNT2	0x10	/* TSU timer count word 2 */
#define ETH_CFG_CLKGEN		0x14	/* TX/RX clock delay + speed override */
#define ETH_CFG_CLK2FC		0x18	/* clock-to-FC delay */
#define ETH_CFG_INTR		0x1c	/* interrupt raw status (write-1-clear) */
#define ETH_CFG_INTE		0x20	/* interrupt enable */
#define ETH_CFG_INTF		0x24	/* interrupt force */
#define ETH_CFG_INTS		0x28	/* interrupt status (masked) */

/*
 * eth_cfg CONTROL register bits (RP-008370-DS-1 Table 134)
 * Bits 31:5 reserved.
 */
#define ETH_CFG_CTRL_MEM_PD		(1u << 4)  /* memory power-down */
#define ETH_CFG_CTRL_BUSERR_EN		(1u << 3)  /* pass MAC AXI bus errors to fabric */
#define ETH_CFG_CTRL_TSU_INC_CTRL	(3u << 1)  /* drives gem_tsu_inc_ctrl[1:0] */
#define ETH_CFG_CTRL_TSU_MS		(1u << 0)  /* drives gem_tsu_ms pin */

/*
 * eth_cfg STATUS register bits (RP-008370-DS-1 Table 135)
 * All fields read-only; reset value 0x04 (RGMII_SPEED=0b10=1Gb, others 0).
 */
#define ETH_CFG_STATUS_AWLEN_ILL	(1u << 5)  /* AXI write AWLEN > 16 beats */
#define ETH_CFG_STATUS_ARLEN_ILL	(1u << 4)  /* AXI read  ARLEN > 16 beats */
#define ETH_CFG_STATUS_DUPLEX		(1u << 3)  /* RGMII_DUPLEX: 1=full */
#define ETH_CFG_STATUS_SPEED_MASK	(3u << 1)  /* RGMII_SPEED: 0=10M,1=100M,2=1G */
#define ETH_CFG_STATUS_SPEED_SHIFT	1
#define ETH_CFG_STATUS_LINK		(1u << 0)  /* RGMII_LINK_STATUS */

/*
 * eth_cfg CLKGEN register bits (RP-008370-DS-1 Table 139)
 * Bits 31:10 reserved.
 * Reset state: ENABLE=1 (bit 7).  If U-Boot stopped the clock generator via
 * KILL or by clearing ENABLE, RGMII STATUS will be frozen.  Always restore
 * ENABLE=1 during module load.
 */
#define ETH_CFG_CLKGEN_TXCLKDELEN	(1u << 9)  /* add delay to rgmii_tx_clk */
#define ETH_CFG_CLKGEN_DC50		(1u << 8)  /* duty-cycle correction for odd divs */
#define ETH_CFG_CLKGEN_ENABLE		(1u << 7)  /* start/stop clock generator cleanly */
#define ETH_CFG_CLKGEN_KILL		(1u << 6)  /* kill clock generator (async) */
#define ETH_CFG_CLKGEN_SPD_FROM_MAC	(3u << 4)  /* RO: speed from MAC */
#define ETH_CFG_CLKGEN_SPD_OVR_EN	(1u << 3)  /* use SPEED_OVERRIDE instead of MAC */
#define ETH_CFG_CLKGEN_SPD_OVR_MASK	(3u << 0)  /* SPD_OVR field: 0=10M, 1=100M, 2=1G */
/* Named values for ETH_CFG_CLKGEN SPD_OVR field */
#define ETH_CFG_CLKGEN_SPD_10M		0u	/* 2.5 MHz RGMII TX clock */
#define ETH_CFG_CLKGEN_SPD_100M		1u	/* 25 MHz RGMII TX clock */
#define ETH_CFG_CLKGEN_SPD_1G		2u	/* 125 MHz RGMII TX clock */

/*
 * eth_cfg CLK2FC SEL field (RP-008370-DS-1 Table 140)
 * Bits 1:0: 0=NONE, 1=rgmii_tx_clk, 2=rgmii_rx_clk
 */
#define ETH_CFG_CLK2FC_SEL_NONE		0u
#define ETH_CFG_CLK2FC_SEL_TX		1u
#define ETH_CFG_CLK2FC_SEL_RX		2u

/* eth_cfg INTE / INTR bits — IEEE 1588 TSU interrupts (Table 141) */
#define ETH_CFG_INT_TSU_CMP		(1u << 12)
#define ETH_CFG_INT_SOF_RX		(1u << 11)
#define ETH_CFG_INT_SYNC_RX		(1u << 10)
/* ... additional PTP bits 9:3 ... */
#define ETH_CFG_INT_GEM			(1u << 0)  /* aggregated GEM interrupt */

/* -----------------------------------------------------------------------
 * RP1 GPIO constants for PHY reset (GPIO 32, active-low)
 *
 * Bank layout (from bcm2712_var.h):
 *   Bank 0: GPIO  0-27  base offset 0x0000
 *   Bank 1: GPIO 28-33  base offset 0x4000
 *   Bank 2: GPIO 34-53  base offset 0x8000
 *
 * GPIO32 → bank 1, index = 32-28 = 4
 * CTRL register = GPIO_BASE + 0x4000 + 4*8 + 4 = GPIO_BASE + 0x4024
 *
 * RP1 GPIO_CTRL register bits (verify against RP-008370-DS-1 §5):
 *   [4:0]  FUNCSEL  (0=ALT0, 5=SIO/GPIO software control)
 *   [9:8]  OUTOVER  (0=normal, 1=invert, 2=force-low, 3=force-high)
 *   [11:10] OEOVER  (0=normal, 1=invert, 2=force-disable, 3=force-enable)
 * ----------------------------------------------------------------------- */
#define RP1_ETH_PHY_RESET_GPIO		32
#define RP1_ETH_PHY_RESET_ACTIVE_LOW	1
#define RP1_ETH_PHY_RESET_PULSE_US	5000   /* 5 ms reset assertion */
#define RP1_ETH_PHY_MDIO_READY_MS	150    /* BCM PHY MDIO ready after reset */

/*
 * RP1 GPIO CTRL override fields — CORRECT positions from RP-008370-DS-1 §3.1.4
 * Table 8.  These differ from RP2040 where OUTOVER is at bits [9:8].
 *
 * [13:12] OUTOVER: 0=from peri, 1=inv peri, 2=force-LOW, 3=force-HIGH
 * [15:14] OEOVER:  0=from peri, 1=inv peri, 2=disable,   3=enable
 */
#define RP1_GPIO_OUTOVER_MASK		(3u << 12)
#define RP1_GPIO_OUTOVER_NORMAL		(0u << 12)  /* output from peripheral */
#define RP1_GPIO_OUTOVER_LOW		(2u << 12)  /* force output LOW */
#define RP1_GPIO_OUTOVER_HIGH		(3u << 12)  /* force output HIGH */

#define RP1_GPIO_OEOVER_MASK		(3u << 14)
#define RP1_GPIO_OEOVER_NORMAL		(0u << 14)  /* OE from peripheral */
#define RP1_GPIO_OEOVER_FORCE_EN	(3u << 14)  /* force output-enable ON */

/* -----------------------------------------------------------------------
 * FDT compatible strings
 * ----------------------------------------------------------------------- */
#define RP1_ETH_COMPAT_PRIMARY		"raspberrypi,rp1-gem"
#define RP1_ETH_COMPAT_SECONDARY	"cdns,macb"
#define RP1_ETH_PHY_MODE_RGMII_ID	"rgmii-id"

/* Compatible strings for Pi 5 root node variants */
#define RPI5_COMPAT_5B			"raspberrypi,5-model-b"
#define RPI5_COMPAT_CM5			"raspberrypi,5-compute-module"

/* -----------------------------------------------------------------------
 * Softc — Milestone 1 (eth_cfg bring-up + link observation)
 *
 * Milestone 2 will extend this with MAC window KVA, ifnet, miibus etc.
 * ----------------------------------------------------------------------- */
struct rp1_eth_softc {
	struct mtx	 sc_mtx;

	/* GEM MAC MMIO window (needed to enable RXEN for RGMII state machine) */
	void		*mac_kva;		/* KVA from pmap_mapdev_attr */
	int		 mac_mapped;

	/* eth_cfg MMIO window */
	void		*cfg_kva;		/* KVA from pmap_mapdev_attr */
	int		 cfg_mapped;

	/* FDT-sourced metadata */
	uint8_t		 mac_addr[6];		/* local-mac-address */
	char		 phy_mode[32];		/* phy-mode string */
	uint32_t	 phy_addr;		/* MDIO address from ethernet-phy@N.reg */
	uint32_t	 phy_reset_gpio;	/* GPIO line for PHY reset */
	uint32_t	 phy_reset_flags;	/* 1 = active-low */

	/* sysctl */
	struct sysctl_ctx_list sysctl_ctx;
	struct sysctl_oid     *sysctl_tree;
};

/* Lock helpers */
#define RP1_ETH_LOCK(sc)	mtx_lock(&(sc)->sc_mtx)
#define RP1_ETH_UNLOCK(sc)	mtx_unlock(&(sc)->sc_mtx)
#define RP1_ETH_LOCK_ASSERT(sc)	mtx_assert(&(sc)->sc_mtx, MA_OWNED)

/* -----------------------------------------------------------------------
 * Milestone 2 attach/detach interface (rp1_eth.c ↔ rp1_eth_cfg.c)
 * ----------------------------------------------------------------------- */
int  rp1eth_attach(struct rp1_eth_softc *cfg_sc);
void rp1eth_detach(void);

/* eth_cfg register accessors (no lock needed for read-only diagnostic) */
#define CFG_RD4(sc, reg) \
	(*(volatile uint32_t *)((uintptr_t)(sc)->cfg_kva + (reg)))

#endif /* _RP1_ETH_VAR_H_ */
