# RP1 Ethernet Hardware Block Diagram

## Data path: copper cable → AXI bus fabric

```
 RJ45
  │
  │  MDI (differential pairs)
  ▼
┌─────────────────────────────────┐
│         BCM54213PE              │
│         (Copper PHY)            │
│                                 │
│  ┌───────────┐  ┌────────────┐  │
│  │ 1000BASE-T│  │ RGMII I/F  │  │
│  │ PCS / PMA │  │            │  │
│  │           │  │ TX delay:  │  │
│  │  autoneg  │  │  GTXCLK_EN │  │
│  │  AN/LT    │  │ RX delay:  │  │
│  └───────────┘  │  SKEW_EN   │  │
│                 │ DLL: off   │  │
│                 │ (DLLAPD=1) │  │
└─────────────────┼────────────┼──┘
                  │            │
    RXCLK (125 MHz, from PHY)  │
    RXD[3:0], RX_CTL           │   MDC / MDIO
    TXD[3:0], TX_CTL           │◄──────────────────────────────┐
    TXCLK (125 MHz, from MAC)  │                               │
                  │            │                               │
                  ▼            │                               │
┌─────────────────────────────────────────────────────────────┐│
│                       RP1  (PCIe2 endpoint)                 ││
│                 RP1-internal APB address space              ││
│                                                             ││
│  ┌────────────────────────────────────────────────────────┐ ││
│  │                eth_cfg  (APB slave)                    │ ││
│  │                phys 0x1f_00104000                      │ ││
│  │                                                        │ ││
│  │  offset 0x00  CONTROL   (bus-err enable, TSU ctrl)     │ ││
│  │  offset 0x04  STATUS    (RGMII_LINK, SPEED, DUPLEX)    │ ││
│  │  offset 0x14  CLKGEN    (generates TXCLK for PHY)      │ ││
│  │               ├─ ENABLE / KILL                         │ ││
│  │               ├─ SPD_OVR_EN + SPD_OVR (0=10M,1=100M,2=1G)││
│  │               └─ SPD_FROM_MAC (RO, tracks NET_CFG)     │ ││
│  │  offset 0x1c  INTR      (SOF_RX, GEM aggregated)       │ ││
│  │  offset 0x20  INTE      (interrupt enable)              │ ││
│  └──────────────────────────┬─────────────────────────────┘ ││
│                             │                               ││
│                    GMII (internal, 8-bit parallel)          ││
│                    + RGMII↔GMII conversion in eth_cfg       ││
│                             │                               ││
│  ┌──────────────────────────▼─────────────────────────────┐ ││
│  │            Cadence GEM_GXL 1p09  (APB slave + AXI mst) │ ││
│  │                phys 0x1f_00100000                       │ ││
│  │                                                        │ ││
│  │  NET_CTRL  0x000   RE(bit2) TE(bit3) MDIO_EN(bit4)     │ ││
│  │  NET_CFG   0x004   GIGE_EN  FULL_DUPLEX  MDC_DIV       │ ││
│  │  NET_STAT  0x008   MDIO_IDLE  PCS_LINK                 │ ││
│  │  USRIO     0x00c   RGMII(bit0) — write-only            │ ││
│  │  PHY_MAINT 0x034   Clause-22 MDIO frame register       │─┘│
│  │                                                        │  │
│  │  ┌──────────────────┐   ┌────────────────────────────┐ │  │
│  │  │  RX descriptor   │   │  TX descriptor ring /      │ │  │
│  │  │  ring / DMA      │   │  DMA engine                │ │  │
│  │  └────────┬─────────┘   └────────────┬───────────────┘ │  │
│  └───────────┼──────────────────────────┼─────────────────┘  │
│              │   AXI master             │                     │
│              └──────────┬───────────────┘                     │
│                         │                                     │
│               RP1 internal AXI fabric                        │
│                         │                                     │
│  ┌──────────────────────▼──────────────────────────────────┐ │
│  │            PCIe2 outbound window                        │ │
│  │   CPU phys 0x1f_00000000  →  PCIe addr 0x0000_0000     │ │
│  │   (RP1 maps:  PCIe 0  →  RP1-internal 0xc0_4000_0000)  │ │
│  └──────────────────────┬──────────────────────────────────┘ │
└─────────────────────────┼─────────────────────────────────────┘
                          │  PCIe Gen 2 ×4
                          ▼
            ┌─────────────────────────────┐
            │          BCM2712 SoC         │
            │                             │
            │  ┌──────────────────────┐   │
            │  │  pcie2 root complex  │   │
            │  │  (host bridge)       │   │
            │  └──────────┬───────────┘   │
            │             │               │
            │  ┌──────────▼───────────┐   │
            │  │   system interconnect│   │
            │  │   + IOMMU            │   │
            │  └──────────┬───────────┘   │
            │             │               │
            │  ┌──────────▼───────────┐   │
            │  │  ARM Cortex-A76 ×4   │   │
            │  │  (aarch64)           │   │
            │  └──────────────────────┘   │
            └─────────────────────────────┘
```

## Signal directions

| Signal group          | Direction         | Notes                                          |
|-----------------------|-------------------|------------------------------------------------|
| MDI (RJ45 pairs)      | bidirectional     | 1000BASE-T, differential                       |
| TXCLK (RGMII)         | MAC → PHY         | 125 MHz at 1G; generated by eth_cfg CLKGEN     |
| TXD[3:0] + TX_CTL     | MAC → PHY         | DDR on TXCLK rising+falling edges at 1G        |
| RXCLK (RGMII)         | PHY → MAC         | 125 MHz at 1G; recovered by BCM54213PE         |
| RXD[3:0] + RX_CTL     | PHY → MAC         | DDR; in-band status on RXD during IPG          |
| MDC                   | MAC → PHY         | Management clock, ÷96 of GEM pclk (~200 MHz)  |
| MDIO                  | bidirectional     | Management data; driven via GEM PHY_MAINT reg  |
| GMII (internal)       | eth_cfg ↔ GEM     | 8-bit parallel inside RP1; converted by eth_cfg|
| AXI (DMA)             | GEM master        | GEM fetches/stores packet data via AXI         |
| APB (config)          | CPU slave         | Register access from BCM2712 via PCIe2         |

## RGMII in-band status (eth_cfg.STATUS)

During the inter-packet gap (RX_CTL = 0), BCM54213PE drives:

```
  RXD[3] = DUPLEX      (1 = full)
  RXD[2:1] = SPEED     (0b10 = 1G, 0b01 = 100M, 0b00 = 10M)
  RXD[0] = LINK        (1 = link up)
```

eth_cfg latches these into STATUS[3:0] on each RXCLK edge during the IPG.
Linux (macb driver) does not use this register; it detects link via MDIO/phylink.

## Physical address map (BCM2712 CPU view)

```
  0x1f_00100000  +0x0000  GEM_GXL NET_CTRL        (r/w)
                 +0x0004  GEM_GXL NET_CFG          (r/w)
                 +0x0008  GEM_GXL NET_STATUS       (ro)
                 +0x000c  GEM_GXL USRIO            (wo)
                 +0x0034  GEM_GXL PHY_MAINT/MDIO   (r/w)

  0x1f_00104000  +0x0000  eth_cfg CONTROL          (r/w)
                 +0x0004  eth_cfg STATUS            (ro)
                 +0x0014  eth_cfg CLKGEN            (r/w)
                 +0x001c  eth_cfg INTR              (w1c)
                 +0x0020  eth_cfg INTE              (r/w)
```

## BCM54213PE shadow register state for rgmii-id

```
  reg 0x18, shadow 7 (AuxCtl MISC):
      bit 8  RGMII_SKEW_EN = 1   (RXC-RXD clock skew enabled)
      bit 7  RGMII_EN      = 1   (RGMII mode)
      read:  write 0x7007 to reg 0x18, then read (bits[14:12]=7 selects shadow)
      write: write 0x8000 | 0x0007 | val  (WREN=bit15, shadow=bits[2:0])

  reg 0x1c, shadow 3 (BCM54810_SHD_CLK_CTL):
      bit 9  GTXCLK_EN = 1       (internal TX clock delay)
      read/write: shadow select in bits[14:10]

  reg 0x1c, shadow 5 (SCR3):
      bit 1  DLLAPD_DIS = 1      (DLL powered down; SKEW_EN used for RX delay)
```
