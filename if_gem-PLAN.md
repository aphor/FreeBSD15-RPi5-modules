# if_gem / rp1_eth KLD Plan

Plan for a new FreeBSD KLD that brings up the Raspberry Pi 5's on-board Gigabit
Ethernet. The RP1 southbridge integrates a Cadence `GEM_GXL 1p09` MAC; FreeBSD
already ships a driver for this IP family in `sys/dev/cadence/if_cgem.c`
(currently attached to Zynq / ZynqMP / PolarFire / SiFive via FDT simplebus).
Our job is to drive that same MAC on a platform where no FreeBSD PCIe root
complex driver exists, reusing as much of `if_cgem.c` as possible.

## Hardware facts (from RP-008370-DS-1 ch. 7 and the live FDT)

- **MAC IP:** Cadence `GEM_GXL 1p09`, 10/100/1000 RGMII, AXI4 master, IEEE 1588
  TSU, 4 address filters, jumbo to 16383, DMA store-and-forward.
- **Register windows (RP1 bus → CPU phys via pcie2 outbound window):**
  - `eth` MAC: RP1 `0xc0_40100000` size `0x4000` → CPU phys `0x1f_40100000`
  - `eth_cfg` glue: RP1 `0xc0_40104000` size `0x1000` → CPU phys `0x1f_40104000`
- **`eth_cfg` registers:** CONTROL, STATUS, TSU_TIMER_CNT[0..2], CLKGEN (RGMII
  TX/RX delays + speed override), CLK2FC, INTR / INTE / INTF / INTS.
- **FDT node:** `/axi/pcie@120000/rp1/ethernet@100000`,
  `compatible = "raspberrypi,rp1-gem", "cdns,macb"`.
- **phy-mode:** `rgmii-id` — BCM PHY applies both TX and RX delays internally;
  `eth_cfg.CLKGEN.{TXCLKDELEN,RXCLKDELEN}` MUST stay cleared.
- **local-mac-address:** firmware-stamped on this board (`88:a2:9e:79:49:6c`).
- **PHY:** `ethernet-phy@1` (MDIO addr 1), Broadcom family (likely BCM54213PE,
  identified at runtime via MII).
- **PHY reset:** phandle `0x2e`, GPIO line 32, active-low, 5 ms duration.
- **Interrupt:** GIC SPI `<6 4>` aggregated through the pcie2 root complex.
- **RGMII clock:** 125 MHz from the RP1 core PLL (datasheet §2.5.3).

## Design decisions (locked in)

1. **Reuse `if_cgem.c` by forking it into the KLD.** The attach frontend is
   replaced; everything downstream of `bus_read_4` / `bus_write_4` stays
   verbatim. When upstream gains fixes we rebase the fork.
2. **Polling first.** No interrupts until the MAC and ring plumbing are proven.
3. **MAC address from FDT `local-mac-address`.** Fallback to an LAA derived
   from the board serial only if the property is absent.
4. **FDT discovery, `pmap_mapdev_attr` mapping.** The FDT gives us metadata
   (MAC, phy-mode, phy addr, phy-reset-gpios); the mapping itself bypasses
   FDT and goes straight through the pcie2 outbound window the VPU firmware
   has already configured, following the same pattern as the RP1 PWM work in
   `bcm2712.c`.
5. **Module layout.** Mirror the existing `bcm2712` + `rpi5` split:
   ```
   rp1_eth.c        // forked from if_cgem.c, attach frontend replaced
   rp1_eth_hw.h     // verbatim copy of if_cgem_hw.h (BSD-2-Clause preserved)
   rp1_eth_cfg.c    // eth_cfg + pinmux + FDT introspection (new code)
   rp1_eth_var.h
   Makefile.rp1_eth
   ```
   `MODULE_DEPEND(rp1_eth, bcm2712, 1, 1, 1)` and `MODULE_DEPEND(rp1_eth,
   miibus, 1, 1, 1)`.

---

## Milestone 1 — `eth_cfg` bring-up and link observation

**Goal:** observe `eth_cfg.STATUS.RGMII_LINK_STATUS` flip on cable events
without touching the MAC DMA paths. Validates mapping, glue programming,
pinmux, and PHY reset.

1. New files: `rp1_eth_cfg.c`, `rp1_eth_var.h`, `Makefile.rp1_eth`. Add
   targets to the top-level `Makefile`.
2. Module init:
   - Verify root compatible is a Pi 5 variant
     (`raspberrypi,5-model-b` / `raspberrypi,5-compute-module*`).
   - Walk FDT to find `raspberrypi,rp1-gem`; read `local-mac-address`,
     `phy-mode`, `phy-handle` → `ethernet-phy@1.reg`, `phy-reset-gpios`.
     Store in softc.
   - Refuse attach cleanly if `phy-mode` ≠ `rgmii-id` (can be relaxed later).
3. Map `eth_cfg` (`0x1f_40104000`, `0x1000`). Expose read-only sysctls
   `dev.rp1_eth.0.cfg.{control,status,clkgen,clk2fc,intr}`.
4. PHY reset:
   - Resolve `phy-reset-gpios` phandle to its RP1 GPIO bank; compute CTRL
     register offset for line 32 (bank `{28,6,0x4000}` → index `32-28=4`).
   - Set pin FUNCSEL to software GPIO output (same pinmux helper as the
     existing GPIO45 → PWM1 code in `bcm2712.c`).
   - Drive low (active-low per flags), wait 5 ms, release, wait ≥150 ms for
     BCM PHY MDIO readiness.
5. No MAC bring-up, no DMA, no cgem fork in this milestone.

**Exit criteria:** plug/unplug the cable, observe
`sysctl dev.rp1_eth.0.cfg.status` showing `RGMII_LINK_STATUS` toggle and
`RGMII_SPEED` / `RGMII_DUPLEX` updating.

---

## Milestone 2 — `if_cgem` in the network stack, polled

**Goal:** `ifconfig rp1_eth0 up && dhclient rp1_eth0` succeeds, ping works,
link events are visible, statistics increment. No interrupts.

### Step 1 — Fork the Cadence driver into the KLD

- Copy `/usr/src/sys/dev/cadence/if_cgem.c` → `rp1_eth.c` and `if_cgem_hw.h` →
  `rp1_eth_hw.h`, preserving the BSD-2-Clause header and adding a
  "forked from rNNNNNN" note.
- Rename public symbols `cgem_*` → `rp1_eth_*` so we can coexist with a
  future `if_cgem` load in the same kernel.
- Delete:
  - `compat_data[]` and all OFW/FDT probe code
  - `cgem_probe`
  - the `DRIVER_MODULE(cgem, simplebus, …)` / `DRIVER_MODULE(miibus, cgem, …)`
    lines
  - `ofw_bus_get_node`, `OF_getprop`, `phy-handle` walking, and `phy-mode`
    parsing (already done in `rp1_eth_cfg.c` in milestone 1 — pass the
    results in via softc)
- Keep unchanged: ring init/teardown, `rp1_eth_init_locked`,
  `rp1_eth_start_locked`, TX/RX descriptor handling, `rp1_eth_stats_update`,
  media/ifmedia callbacks, ioctl handler, `PHY_MAINT` MDIO helpers, and the
  `rp1_eth_poll_locked()`-equivalent path cgem already has for driving RX/TX
  completion from a softclock.

### Step 2 — Module-init attach frontend

Add `rp1_eth_mod_attach(void)` invoked from `MOD_LOAD` in `rp1_eth_cfg.c`
after milestone 1 has mapped `eth_cfg`, pulsed PHY reset, and gathered FDT
metadata:

```c
struct rp1_eth_softc *sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
sc->mac_kva  = pmap_mapdev_attr(0x1f40100000, 0x4000, VM_MEMATTR_DEVICE);
sc->cfg_kva  = /* already mapped in milestone 1 */;
sc->phy_addr = fdt_phy_addr;           /* = 1 */
sc->phy_mode = RP1_ETH_PHY_MODE_RGMII_ID;
memcpy(sc->macaddr, fdt_local_mac, 6);
```

- Fabricate a `struct resource` shim whose `r_bustag` is `fdtbus_bs_tag`
  (ARMv8 device memory tag) and whose `r_bushandle` is
  `(bus_space_handle_t)sc->mac_kva`. All `bus_read_4` / `bus_write_4` calls
  in the forked body then work unchanged (~30 lines of shim).
- Create an `ifnet` without a device_t parent:
  `sc->ifp = if_alloc(IFT_ETHER); if_initname(sc->ifp, "rp1_eth", 0);`
  Wire `if_ioctl`, `if_transmit`, `if_qflush`, `if_init`, `if_start` from
  the forked driver.
- `ether_ifattach(sc->ifp, sc->macaddr);`

### Step 3 — miibus without newbus

miibus wants a real `device_t` parent. Two options, in order of preference:

**(a) Synthesize a minimal device_t.** Use
`device_add_child(root_bus, "rp1_eth", -1)` at MOD_LOAD, let newbus allocate
a real `device_t`, attach `miibus` as its child via
`device_add_child(dev, "miibus", -1)` + `bus_generic_attach`. All of
miibus's lifecycle, `mii_attach`, and `mii_tick` remain intact. We still
never register with OFW — the device_t is a leaf on `nexus` / `root_bus`.

**(b) Hand-rolled PHY** — skip miibus, talk directly to the PHY through
GEM's `PHY_MAINT`. Reimplements autoneg; not worth it.

Choose (a). The forked cgem body already calls
`mii_attach(dev, &sc->miibus, …)`; hand it the synthesized device_t.

### Step 4 — Polled RX/TX

- `sc->poll_callout` runs `rp1_eth_poll_locked(sc)` every `hz/200` (5 ms).
- `rp1_eth_poll_locked` runs cgem's RX reclaim and TX reclaim back-to-back,
  followed by `rp1_eth_start_locked` if the TX queue has drainable packets.
- `if_transmit` enqueues to the driver's software `buf_ring` (existing cgem
  path), flags "work pending"; the callout performs the DMA kick and
  descriptor writeback walk.
- `mii_tick` every second (separate callout, or piggyback on the poll
  callout's second boundary).
- **Interrupts stay masked.** Explicitly write `0xffffffff` to MAC
  `INT_DISABLE` and `0` to `eth_cfg.INTE` at init. Keep `rp1_eth_intr()`
  present but unreferenced, marked `/* TODO: milestone 3 */`.

### Step 5 — Sysctls and observability

Under `dev.rp1_eth.0.`:

- `status.link`, `status.speed`, `status.duplex` (from miibus, cross-checked
  against `eth_cfg.STATUS.RGMII_*`)
- `stats.*` — wire up cgem's existing statistics block
- `poll.interval_us` — tunable, default 5000
- `poll.iterations`, `poll.rx_reclaimed`, `poll.tx_reclaimed` — counters
  confirming the callout is doing work
- `debug.dump_desc_rings` — on-demand ring dump

### Step 6 — Bring-up verification order

1. MAC self-identifies: `HW_ID` / `USER_IO` reads return Cadence signature,
   not `0xffffffff`.
2. MDIO read of PHY ID at addr 1 returns a Broadcom OUI.
3. `mii_attach` succeeds, `ifconfig rp1_eth0` shows media list.
4. Cable plug/unplug: miibus media callback fires, `eth_cfg.STATUS` mirrors.
5. `ifconfig rp1_eth0 up`: RX ring fills, no descriptor errors, no AXI
   errors in `eth_cfg.STATUS.{AWLEN,ARLEN}_ILLEGAL`.
6. `ping <gateway>`: ARP + ICMP both ways. **First real packet is the
   milestone gate.**
7. `dhclient rp1_eth0`: end-to-end validation.
8. `iperf3` smoke test — expect capped throughput from 5 ms polling; log
   the number to motivate milestone 3.

### Step 7 — Detach cleanliness

`MOD_UNLOAD`: stop callout → `ether_ifdetach` → tear down miibus → free
rings → `pmap_unmapdev` both windows → free synthesized device_t.
Idempotent: `kldunload rp1_eth && kldload rp1_eth` must round-trip cleanly.

### Risk log — milestone 2

- **AXI bus errors from MAC before rings exist.** `eth_cfg.CONTROL.BUSERR_EN`
  default is `0` (errors swallowed) — leave it there during bring-up.
- **Cache coherency.** RP1 DMA via pcie2 is coherent with A76 on Pi 5;
  `BUS_DMA_COHERENT` should be safe. If unsure, keep `bus_dmamap_sync`
  calls — they're cheap.
- **Jumbo and checksum offloads.** Disabled initially
  (`IFCAP_*` cleared at attach). Revisit after milestone 3.
- **Multi-queue.** Single TX / single RX queue only. GEM_GXL supports
  priority queues; cgem's `HWQUIRK_NEEDNULLQS` sets up NULL descriptors for
  the unused queues. Copy that path verbatim if register reads show extra
  queues enabled.

---

## Milestone 3 — real interrupts via pcie2

**Goal:** delete the polling callout; RX/TX completion and link events
come from a hard IRQ. Throughput jumps toward line rate; CPU overhead drops
to interrupt-driven levels.

FreeBSD has no driver bound to BCM2712 pcie2. Either write a minimal one,
or take a shortcut.

### Option A (fallback) — 1 kHz `eth_cfg.INTR` polling

Tighten the milestone 2 callout to `hz = 1000` and gate on `eth_cfg.INTR`
being non-zero (99% of ticks are one MMIO read + return). "Poor man's
interrupts," fallback only, not the target.

### Option B (target) — minimal `bcm2712_pcie` host controller KLD

Sibling module to `bcm2712`, doing just enough to route one interrupt from
the RP1 Cadence GEM to a FreeBSD `intr_*` handler.

#### 3.1 Reconnaissance: what has the VPU firmware left us?

Before writing code:

- Map pcie2 RC config + DBI window (CPU phys from the `pcie@120000` node's
  `reg`).
- Dump LINK_STATUS, LTSSM state, BAR0, outbound window registers, MSI
  controller enables, INTx mask.
- Walk config space of the downstream RP1 endpoint; confirm BAR layout
  matches the outbound window exposing `0x1f_00000000`.
- Determine whether MSI / MSI-X is already enabled by firmware.

Short writeup of "what's live" vs "what we must program." Likely outcomes:

- **Best case:** firmware enabled MSI at a known doorbell address; we
  allocate a vector, program the EP's MSI capability, hook `intr_pic_*`
  at the RC side.
- **Middle case:** firmware left MSI controller off but link is up and
  outbound windows are configured; we program the MSI controller ourselves.
- **Worst case:** no usable MSI hardware without full RC init; fall back to
  legacy INTx via the DWC legacy interrupt aggregator (one shared line
  demuxed by reading per-device status).

#### 3.2 Write `bcm2712_pcie` (minimum viable)

Strictly scoped to what RP1 GEM needs:

- `MOD_LOAD`: `pmap_mapdev_attr` DWC control registers (RC DBI + MSI
  controller + outbound/inbound window config).
- Verify LTSSM is L0 and link is up; refuse to attach if not.
- Register a FreeBSD `pic`: implement enough of `pic_if.m` to expose a
  single IRQ source labelled "rp1-eth" supporting `PIC_ENABLE_SOURCE`,
  `PIC_DISABLE_SOURCE`, `PIC_PRE_ITHREAD`, `PIC_POST_ITHREAD`. References:
  `sys/arm64/broadcom/brcmstb/brcmstb_pcib.c` (if present for BCM2711),
  otherwise `sys/dev/pci/pci_host_generic.c`.
- Hard IRQ is routed to a GIC SPI (visible in the pcie2 FDT node's
  `interrupts` property). Hook that SPI via `bus_setup_intr` on nexus at
  attach; in the handler read MSI / legacy aggregator status and dispatch
  to the child ISR registered by `rp1_eth`.
- Export `bcm2712_pcie_register_rp1_intr(ih_fn, arg)` as a small KPI,
  skipping the full newbus PCIe bus dance. This is not a general-purpose
  PCIe host driver — it's a single-wire router.
- `MODULE_DEPEND(rp1_eth, bcm2712_pcie, 1, 1, 1)`.

#### 3.3 Wire `rp1_eth_intr`

In the forked `rp1_eth.c`, un-ifdef the ISR body:

```c
bcm2712_pcie_register_rp1_intr(rp1_eth_intr, sc);
/* unmask MAC-level sources */
WR4(sc, GEM_INT_EN, RX_COMPLETE | RX_USED_READ | TX_COMPLETE | TX_USED_READ |
                    HRESP_NOT_OK | RX_OVERRUN);
/* unmask eth_cfg aggregation */
WR4_CFG(sc, ETH_CFG_INTE, ETH_CFG_INTE_ETHERNET);
```

Delete the polling callout (keep `mii_tick` on its own 1 Hz callout).
Retain a compile-time `RP1_ETH_POLLING` option that re-enables milestone 2
behavior for A/B debugging — will pay for itself the first time we chase
a missed interrupt.

#### 3.4 Validation

1. `vmstat -i` shows `rp1_eth` ticking; rate matches RX packet rate under
   load.
2. `iperf3` approaches line rate for 1 Gbps; compare to milestone 2 polling.
3. Load test: `ping -f` + `iperf3` + link flap; verify no missed interrupts
   (raw `eth_cfg.INTR` vs serviced counters).
4. Unload/reload stress: `while :; do kldload rp1_eth; kldunload rp1_eth;
   done` — interrupt hookup/teardown is the most common panic source.
5. `netstat -ss` error counters equal or better than milestone 2 baseline.

#### 3.5 Risk log — milestone 3

- **PCIe RC in unknown state.** Recon step 3.1 exists to de-risk this.
  If firmware state is unusable, either program the RC from scratch (large
  scope) or fall back to Option A.
- **MSI vs INTx.** MSI is cleaner; INTx requires demuxing shared lines.
  Pick based on 3.1 findings.
- **Affinity, coalescing, RSS.** Not in scope. Single CPU. Throughput is
  the goal; latency variance is not.
- **IOMMU / SMMU.** Pi 5 pcie2 does not use the SMMU for RP1
  (firmware 1:1 windows). Revisit only if corruption is observed.
- **Future FreeBSD PCIe RC driver conflict.** If someone writes a proper
  `bcm2712_pcib` for `pci(4)`, this single-wire shim will conflict.
  Document loudly in `bcm2712_pcie`'s header so the conflict is obvious.

### Exit criteria — milestone 3

- `sysctl dev.rp1_eth.0.poll.*` no longer exists (polling path removed
  under `!RP1_ETH_POLLING`).
- `iperf3 -c <peer>` hits ≥ 900 Mbps sustained TX and RX on a quiet Pi 5.
- 24-hour `ping -i 0.01` + background `iperf3` with zero lost packets and
  zero interrupt storms.
- Module unload/reload succeeds 1000× in a row.
