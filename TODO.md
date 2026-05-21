# cyw43455 KLD Plan

Plan for a new FreeBSD KLD that brings up WiFi (802.11ac) and Bluetooth 5.0
on the Raspberry Pi 5 using its on-board Infineon/Cypress CYW43455 combo
radio. The chip communicates over SDIO v3.0 (WLAN) and UART HCI (Bluetooth),
using Broadcom's proprietary FullMAC firmware architecture: the host driver
sends high-level commands (IOCTLs/IOVARs) and the ARM Cortex-R4 on the chip
runs the real 802.11 state machine.

The `freebsd-brcmfmac` repository (`../freebsd-brcmfmac.git`) provides a
working reference implementation for both PCIe (BCM4350) and SDIO (BCM43455)
paths on FreeBSD. This plan adapts the SDIO path into the `rpi5_modules`
KLD framework.

Reference: Infineon CYW43455 datasheet 002-15051 Rev. *O
Reference: `../freebsd-brcmfmac.git` (SDIO + BCDC + net80211 integration)

## Hardware facts

### CYW43455 chip overview (datasheet 002-15051)

- **Silicon:** Single-chip 802.11a/b/g/n/ac MAC/baseband/radio + BT 5.0 + EDR.
- **WLAN CPU:** ARM Cortex-R4, 800 KB SRAM + 704 KB ROM. Runs FullMAC firmware.
- **BT CPU:** ARM Cortex-M3, 270 KB RAM + 845 KB ROM. Runs HCI stack.
- **WLAN host interface:** SDIO v3.0 (F0/F1/F2), 4-bit, up to DDR50.
  PCIe interface exists on-die but is **not brought up** by Cypress firmware.
- **BT host interface:** 4-wire UART (HCI H4/H5), up to 4 Mbps.
  BT_HOST_WAKE / BT_DEV_WAKE for out-of-band sleep signaling.
- **Power control:** WL_REG_ON (WLAN power + reset), BT_REG_ON (BT power +
  reset). Both active-high with internal 200 kOhm pull-downs.
- **Coexistence:** On-chip GCI arbitrates WLAN/BT medium access. External
  coexistence interface available but unused on Pi 5.
- **Strapping:** GPIO_16 pulled high at POR selects SDIO (not PCIe).
  GPIO_7 selects SDIO I/O voltage (default 1.8 V).

### Pi 5 board connectivity

- **SDIO controller:** BCM2712-internal `sdio2` at `0x10_01100000`
  (`compatible = "brcm,bcm2712-sdhci"`). This is **not** routed through the
  RP1 southbridge — it's a native BCM2712 peripheral.
- **FDT path:** `/axi/mmc@1100000/wifi@1`
  (`compatible = "brcm,bcm4329-fmac"`).
- **WLAN power:** `wl_on_reg` regulator-fixed, GPIO 28 (gio_aon), 3.3 V,
  150 ms startup delay.
- **BT UART:** `uarta` (BCM2712 mini-UART or PL011), BT shutdown on GPIO 29.
- **Bus width:** 4-bit SDIO, UHS DDR50 capable.
- **MAC address:** Firmware-stamped in FDT `local-mac-address` property,
  or read from OTP.

### SDIO function layout (datasheet §9.1)

| Function | Purpose                        | Max block size |
|----------|--------------------------------|----------------|
| F0       | Standard SDIO (CIA)            | 32 B           |
| F1       | Backplane (SoC register space) | 64 B           |
| F2       | WLAN packet DMA                | 512 B          |

### Firmware files (loaded from `/boot/firmware/`)

| File                          | Purpose                              |
|-------------------------------|--------------------------------------|
| `brcmfmac43455-sdio.bin`      | WLAN FullMAC firmware (ARM CR4 code) |
| `brcmfmac43455-sdio.txt`      | NVRAM board config (key=value text)  |
| `brcmfmac43455-sdio.clm_blob` | Regulatory/CLM data                  |
| `BCM4345C0.hcd`               | BT patchram firmware (HCD format)    |

### Protocol stack

```
┌─────────────────────────────────────────────────────────┐
│                  FreeBSD net80211 (wlan)                 │
├─────────────────────────────────────────────────────────┤
│               cyw43455 cfg layer                        │
│     (VAP management, scan, connect, keys, events)       │
├─────────────────────────────────────────────────────────┤
│                      FWIL                               │
│     (firmware IOCTL/IOVAR encoding, bus-agnostic)       │
├─────────────────────────────────────────────────────────┤
│              SDPCM + BCDC framing                       │
│  (12-byte SDPCM header, 16-byte BCDC command header)    │
│  Channels: Control(0), Event(1), Data(2), Glom(3)       │
├─────────────────────────────────────────────────────────┤
│                  SDIO bus layer                          │
│  (F1 backplane access, F2 data transfer, clock mgmt)    │
├─────────────────────────────────────────────────────────┤
│             FreeBSD MMCCAM / SDHCI                       │
│          (bcm2712-sdhci or patched sdhci)               │
├─────────────────────────────────────────────────────────┤
│                CYW43455 hardware                        │
└─────────────────────────────────────────────────────────┘
```

## Design decisions (locked in)

1. **Fork relevant SDIO-path code from `freebsd-brcmfmac`.** The reference
   repo already has working SDIO attach, SDPCM/BCDC protocol, firmware
   download, net80211 integration, scan, and WPA2 connect for BCM43455.
   Fork `sdio.c`, `sdpcm.c`, `fwil.c`, `cfg.c`, `scan.c`, `security.c`,
   and `core.c` into the KLD. Delete all PCIe/msgbuf code paths. Rename
   `brcmf_*` → `cyw_*` to avoid symbol conflicts.

2. **MMCCAM dependency.** The FreeBSD kernel must be built with `MMCCAM`
   enabled and the BCM2712 SDHCI driver must enumerate SDIO functions.
   Kernel patches from `freebsd-brcmfmac.git/patches/` may be needed,
   adapted for the BCM2712 SDHCI controller.

3. **Module layout.** Single KLD initially, split later if needed:
   ```
   cyw43455.c          # MOD_LOAD entry, SDIO probe/attach, power sequencing
   cyw43455_sdio.c     # SDIO backplane access, F1/F2, clock management
   cyw43455_sdpcm.c    # SDPCM/BCDC framing, RX polling, flow control
   cyw43455_fwil.c     # IOCTL/IOVAR encoding (bus-agnostic)
   cyw43455_fw.c       # Firmware + NVRAM + CLM loading via firmware(9)
   cyw43455_cfg.c      # net80211 attach, VAP, link events, state machine
   cyw43455_scan.c     # escan implementation, chanspec conversion (D11N)
   cyw43455_security.c # wsec/wpa_auth, key install, PSK
   cyw43455_var.h      # Softc, constants, register offsets, SDPCM structs
   cyw43455_cfg.h      # net80211-specific structs, event codes
   Makefile.cyw43455
   ```
   `MODULE_DEPEND(cyw43455, bcm2712, 1, 1, 1)` — for GPIO power control.
   `MODULE_DEPEND(cyw43455, firmware, 1, 1, 1)` — for firmware loading.
   `MODULE_DEPEND(cyw43455, wlan, 1, 1, 1)` — for net80211.

4. **WiFi first, Bluetooth later.** WiFi is the primary deliverable.
   Bluetooth HCI over UART is a separate milestone after WiFi works.

5. **Polling first.** SDIO interrupt support on BCM2712 may require kernel
   work. Start with a 50 ms callout polling SDPCM RX, matching the
   reference implementation's approach.

6. **Power sequencing via bcm2712 GPIO.** Use `rp1_gpio` or direct gio_aon
   register access for WL_REG_ON (GPIO 28) and BT_REG_ON (GPIO 29).
   Walk FDT for regulator and GPIO phandle resolution.

7. **Test network.** SSID `VelcroDoggler` (credentials in `.wifi`).
   WPA2-PSK assumed. PSK supplied via sysctl at runtime.

---

## Milestone 0 — Kernel prerequisites and SDIO enumeration

**Goal:** FreeBSD kernel on Pi 5 sees the CYW43455 as an SDIO device and
enumerates F0/F1/F2. No driver code yet — just confirming the bus works.

### Steps

1. **Kernel configuration.** Build a custom kernel with:
   - `options MMCCAM` — CAM-based MMC/SDIO stack
   - `device sdhci` — SDHCI host controller
   - `device mmc` / `device mmcsd` — MMC framework
   - `device wlan` / `device wlan_ccmp` / `device wlan_tkip` — net80211
   - Evaluate whether `bcm2835_sdhci` drives `brcm,bcm2712-sdhci` or if
     a new SDHCI shim is needed.

2. **Apply SDHCI patches.** Port the four patches from
   `freebsd-brcmfmac.git/patches/` to the Pi 5 kernel:
   - `01-bcm2835-sdhci-fix-mmc-fdt-node.patch` — FDT node fix
   - `02-sdhci-platform-update-ios.patch` — power sequencing
   - `03-mmccam-sdio-set-fullspeed-clock-and-4bit-bus-width.patch` — 4-bit
   - `04-sdhci-mask-pio-intr-after-xfer.patch` — PIO interrupt fix

3. **Device tree overlay.** Create or adapt `wlan-pwrseq.dts` for Pi 5
   with correct GPIO phandles for `gio_aon` GPIO 28 (WL_REG_ON).

4. **Boot and verify.** After boot, confirm:
   - `devctl list | grep mmc` shows sdio2 enumerated
   - `camcontrol devlist` or `sdhci` messages show F0/F1/F2 functions
   - `dmesg | grep -i sdio` shows CYW43455 vendor/device IDs

### Exit criteria

SDIO function enumeration visible in `dmesg`. F1 backplane access possible
(vendor ID readable). No kernel panics on SDIO transactions.

### Risk log

- **BCM2712 SDHCI not supported.** The `brcm,bcm2712-sdhci` controller may
  differ from `brcm,bcm2835-sdhci`. May need a new SDHCI platform driver
  or quirks. Mitigation: dump registers, compare with bcm2835 variant.
- **MMCCAM SDIO support incomplete.** FreeBSD's MMCCAM SDIO support is
  relatively new. May hit bugs. Mitigation: reference `freebsd-brcmfmac`
  patches and RPI4-HOWTO.md for known workarounds.
- **Power sequencing.** WL_REG_ON must be driven high before SDIO
  enumeration. If the VPU firmware doesn't do this, we need an early-boot
  GPIO toggle. Mitigation: check if Linux DT `wl_on_reg` regulator is
  already enabled by the VPU.

---

## Milestone 1 — SDIO attach, backplane access, firmware download

**Goal:** KLD loads, probes the CYW43455 via SDIO F1, downloads firmware +
NVRAM + CLM blob, and confirms the chip is alive (firmware version string
readable via IOVAR).

### Step 1 — Module skeleton and power sequencing

New files: `cyw43455.c`, `cyw43455_var.h`, `Makefile.cyw43455`. Add targets
to the top-level `Makefile`.

```c
static int cyw43455_modevent(module_t mod, int type, void *data) {
    switch (type) {
    case MOD_LOAD:
        // 1. Walk FDT to find "brcm,bcm4329-fmac" under sdio2
        // 2. Read local-mac-address from FDT
        // 3. Resolve WL_REG_ON GPIO from regulator phandle
        // 4. Assert WL_REG_ON high, wait 150 ms
        // 5. Proceed to SDIO attach
        break;
    case MOD_UNLOAD:
        // Reverse: detach net80211, stop polling, deassert WL_REG_ON
        break;
    }
}
```

Sysctl tree at `hw.cyw43455.*`.

### Step 2 — SDIO bus layer (`cyw43455_sdio.c`)

Fork from `freebsd-brcmfmac/src/sdio.c`. Delete PCIe ifdefs. Key functions:

- `cyw_sdio_attach()` — find F1/F2 via MMCCAM, set F1 block size = 64,
  F2 block size = 64 (BCM43455 override per reference driver).
- `cyw_sdio_backplane_read/write()` — F1 access to chip registers via
  the Silicon Backplane (SSB/AI) protocol. Window register at
  `0x1000a` sets the 32-bit backplane address window.
- `cyw_sdio_enable_clocks()` — request ALP clock, wait for
  `SBSDIO_FUNC1_CHIPCLKCSR` ready bit.
- `cyw_sdio_f2_ready()` — poll `SDIO_CCCR_IORx` until F2 is ready after
  firmware boot.

### Step 3 — Chip core enumeration (`cyw43455_core.c` or inline)

Fork from `freebsd-brcmfmac/src/core.c`:

- `cyw_chip_enumerate_cores()` — scan EROM table via F1 backplane reads.
  Identify ARM CR4, D11, SDIO cores. Record base addresses and wrapper
  addresses. BCM43455 chip ID = `0x4345`.
- `cyw_chip_enter_download()` — halt ARM CR4, disable D11 core.
- `cyw_chip_exit_download()` — write reset vector, release ARM CR4.

### Step 4 — Firmware download (`cyw43455_fw.c`)

Fork firmware loading from `freebsd-brcmfmac/src/main.c` +
`freebsd-brcmfmac/src/sdio.c`:

- Load `brcmfmac43455-sdio.bin` via `firmware_get()`.
- Write firmware image to chip RAM via F1 backplane writes (bulk).
- Load `brcmfmac43455-sdio.txt`, parse NVRAM text format, write to RAM
  end (with length token).
- `cyw_chip_exit_download()` — boot the firmware.
- Wait for F2 ready (firmware sets F2 ready bit when boot succeeds).
- Send `ver` IOVAR via BCDC to read firmware version string. Print to
  console as confirmation.
- Load `brcmfmac43455-sdio.clm_blob` via `clmload` IOVAR (chunked,
  16 KB per chunk).

### Step 5 — SDPCM/BCDC protocol (`cyw43455_sdpcm.c`)

Fork from `freebsd-brcmfmac/src/sdpcm.c`:

- SDPCM frame format: 4-byte HW header (len, ~len) + 8-byte SW header
  (seq, chan_and_flags, next_len, data_offset, flow_control, credit,
  reserved).
- BCDC command header: 4 fields (cmd, len, flags with IF index, status).
- `cyw_sdpcm_send_ioctl()` — build SDPCM+BCDC frame, write via F2,
  poll for response on control channel.
- `cyw_sdpcm_rx_poll()` — read F2, parse SDPCM header, demux by channel
  (control → IOCTL response, event → firmware events, data → RX packets).
- Start 50 ms polling callout for RX.

### Step 6 — Sysctls and diagnostics

Under `hw.cyw43455.`:

- `firmware_version` — string from `ver` IOVAR
- `chip_id`, `chip_rev` — from EROM scan
- `mac_address` — from FDT or `cur_etheraddr` IOVAR
- `debug` — verbosity level (0=off, 1=info, 2=verbose)
- `sdio.f2_ready` — boolean
- `sdio.backplane_window` — current window address

### Step 7 — Diagnostic tool

Create `tools/cyw43455_status.sh`:
```bash
#!/bin/sh
kldstat | grep cyw43455
sysctl hw.cyw43455 2>/dev/null || echo "module not loaded"
```

Create `tools/cyw43455_load.sh`:
```bash
#!/bin/sh
# Build, install, load, and show status
```

### Exit criteria

- `kldload cyw43455` succeeds without panic.
- `sysctl hw.cyw43455.firmware_version` prints firmware version string
  (e.g., `wl0: Mar 27 2020 05:42:32 version 7.45.206 ...`).
- `sysctl hw.cyw43455.chip_id` shows `0x4345`.
- CLM blob loaded successfully (no `clmload` IOVAR errors).
- `kldunload cyw43455` cleanly tears down and deasserts WL_REG_ON.

### Risk log — milestone 1

- **MMCCAM API differences.** The SDIO function access API
  (`sdio_read_direct`, `sdio_write_direct`, `sdio_read_extended`, etc.)
  may differ between FreeBSD versions. Mitigation: reference the exact
  API used in `freebsd-brcmfmac/src/sdio.c`.
- **F2 timeout.** Firmware may fail to boot if NVRAM is wrong for this
  board. Mitigation: use the exact NVRAM from a working Linux Pi 5
  (`/lib/firmware/brcm/brcmfmac43455-sdio.txt`).
- **Backplane window alignment.** Writes must be window-aligned (8 KB
  windows). The reference driver handles this; fork carefully.
- **Clock management.** The chip needs ALP/HT clocks enabled before
  firmware download. Miss this and F1 reads return `0xffffffff`.

### Status & revised approach — 2026-05-17

**Where we are:** ARM CR4 boots firmware reliably
(`IOCTL=0x00000001 [running]`), code + NVRAM download succeed, SDHCI
stays clean. **Blocker:** F2 never becomes ready (`IORx=0x02` for 30 s);
firmware PMU appears to never finish init.

**Root cause of the stall (was: days of trial-and-error):** the
post-firmware clock handoff was built on a wrong mental model. The
BCM43455 is a **Save/Restore (SR) capable** chip. Key consequences,
fully documented in `_reference/cyw434550_fw.md`:

1. **`SBSDIO_FUNC1_SLEEPCSR` was defined as `0x1000d`; correct is
   `0x1001F`.** Every KSO access hit the wrong register — KSO never
   engaged (`SLEEPCSR` always read `0x00`).
2. **Polling `SBSDIO_HT_AVAIL` (0x80) is wrong for this chip.** On
   SR chips `brcmf_sdio_htclk()` never checks it. HT is *forced* with
   `SBSDIO_FORCE_HT` (0x02), not *requested* with `HT_AVAIL_REQ`
   (0x10). The "no HT" symptom we chased is normal, not a fault.
3. **Missing F2 "kick":** must write
   `SDPCM_PROT_VERSION<<16` (`0x00040000`) to the SDIO core
   `tosbmailboxdata` register *before* `sdio_enable_func(F2)`.
4. **Missing `brcmf_sdio_sr_init()`:** WAKEUPCTRL HTWAIT bit + CCCR
   CARDCAP CMD14 bits + `CHIPCLKCSR = FORCE_HT`, after F2 enable.
5. **`ramsize` is hardcoded guesswork** (`0xe0000`/`0xdc000`). The
   reference driver reads it from the ARM CR4 TCM bank registers.
   Wrong size ⇒ NVRAM mis-placed ⇒ silent PMU stall (our exact
   signature).
6. **F2 watermark** should be `0x40` (435x), not `0x60`.

**Authoritative source identified:** FreeBSD ships the real Linux
brcmfmac at
`/usr/src/sys/contrib/dev/broadcom/brcm80211/brcmfmac/{sdio.c,sdio.h,chip.c}`
on `dunn`. All future work cites that tree by file:line instead of
reconstructing from memory. Distilled into `_reference/cyw434550_fw.md`.

**Revised Step 3 / Step 4 plan (supersedes the EROM-optional shortcut):**

- **3a. EROM core scan is now mandatory, not deferred.** We need real
  base addresses for ChipCommon, **PMU**, **ARM CR4**, and the **SDIO
  core** to: detect SR capability (PMU chipcontrol reg 3 bit 2), read
  true `ramsize` (CR4 TCM banks), and address `tosbmailboxdata`.
- **3b. Fix register map** in `cyw43455_var.h` from
  `_reference/cyw434550_fw.md` §9 (SLEEPCSR, WAKEUPCTRL, FORCE_HT,
  CARDCAP, watermarks, ARMCR4 bank regs).
- **4a. KSO init in attach** (after F1 enable, gate on SDIO core
  rev ≥ 12): read `SLEEPCSR 0x1001F`, set `KSO_EN` if clear.
- **4b. Replace post-release clock block** in `cyw43455_fw.c`:
  drop the `HT_AVAIL_REQ` + `HT_AVAIL` poll entirely. After ARM
  release and `CLK_AVAIL`: `saveclk = read CHIPCLKCSR`,
  write `saveclk | SBSDIO_FORCE_HT`.
- **4c. F2 kick:** write `0x00040000` to SDIO-core
  `tosbmailboxdata` (needs SDIO core base from 3a) before
  `sdio_enable_func(F2)`.
- **4d. Post-F2:** hostintmask, watermark `0x40`, DEVCTL F2WM_ENAB,
  MESBUSYCTRL; then `cyw_sdio_sr_init()` (WAKEUPCTRL HTWAIT +
  CARDCAP + `CHIPCLKCSR=FORCE_HT`).
- **4e. Success test = F2 IORx bit**, not any HT poll.
- **Keep:** CCCR IEN=0 during boot window (verified prevents SDHCI
  cascade); no CMD53 SDIO-core access before ARM release.

**Status as of 2026-05-20: COMPLETE ✅**

| Step | Status | Notes |
|------|--------|-------|
| Firmware download (code + NVRAM) | ✅ | 609 309 bytes + NVRAM @ 0x27392c; readback verified |
| ARM CR4 release | ✅ | IOCTL=0x00000001 RESET_CTL=0x00000000 (running) |
| FORCE_HT clock (SR-chip path) | ✅ | CSR=0x42 (FORCE_HT + ALP_AVAIL) after 65 µs |
| F2 enable | ✅ | `sdio_enable_func(F2)` returns 0; F2 ready |
| SR init (WAKEUPCTRL + CARDCAP + FORCE_HT) | ✅ | SR init done |
| Firmware alive | ✅ | Spontaneous F2 transfer-complete interrupt from firmware |
| FWREADY handshake | ✅ | Poll `HMB_DATA_FWREADY` (bit 3); ~45 ms on reload, ~65 ms cold (commit `1b2f833`) |
| IOVAR "ver" | ✅ | `firmware: wl0: ... version 7.45.265`; SDPCM RX with INTSTATUS gate (commit `d137875`) |
| SDPCM RX callout | ✅ | Per-device taskqueue (`cyw43455_rx`) avoids `taskqueue_thread` deadlock (commit `0ffb512`) |
| `kldunload`/`kldload` 5-cycle test | ✅ | Every cycle: `KSO set`, `fw handshake at ~45 ms`, correct MAC, no panic (commit `eaa8844`) |

**Key lessons learned (documented in `_reference/cyw43455.md` §7–§8):**

- `TOHOSTMAILBOXDATA` retains stale `HMB_DATA_DEVREADY` (0x02) across
  `cyw_arm_halt`. Must poll for `HMB_DATA_FWREADY` (bit 3 = 0x08) specifically.
- INTSTATUS gate in `cyw_sdpcm_recv_one` is **required**. Without it the 50 ms
  callout hammers an empty F2 FIFO, corrupting the SDIO CAM queue and panicking
  (`camq_remove: out-of-bounds index -3`). Gate is safe: INTSTATUS has
  data-available bits set by the time `FWREADY` is written.
- SR capability (PMU CC3 bit 2) is cleared by the running firmware and is not
  restored by `cyw_arm_halt`. This is cosmetic — driver is fully functional
  without SR on reload.
- Global `taskqueue_thread` cannot be used for the SDPCM RX task: `sdiob`
  enqueues device discovery on that thread, so the `cv_timedwait` in
  `cyw_fil_txrx` self-starves. Fixed with a per-device taskqueue.

---

## Milestone 2 — net80211 attach, scan, and WPA2 association

**Goal:** `ifconfig wlan0 create wlandev cyw43455_0 && ifconfig wlan0 up
&& ifconfig wlan0 scan` shows nearby networks. Then:
`ifconfig wlan0 ssid VelcroDoggler && dhclient wlan0` gets an IP address
and `ping` works.

### Current status (2026-05-20)

Steps 1 and 2 are complete. Steps 3–6 are stubs with no functional
implementation. Work order going forward is 3 → 4 → 5 → 6 (each step
depends on the previous; do **not** parallelize — they share `cyw_newstate`
and the event dispatcher, and SDIO still has sharp enough edges that
one-step-at-a-time bisection pays).

| Step | Status | File |
|------|--------|------|
| 1. FWIL layer | ✅ DONE | `cyw43455_fwil.c` |
| 2. net80211 attach scaffolding | ✅ DONE | `cyw43455_cfg.c` |
| 3. Event dispatcher | ⬜ not started | `cyw43455_events.c` (new) |
| 4. Escan | ⬜ not started | `cyw43455_scan.c` (new) |
| 5. Association + WPA2 | ⬜ not started | `cyw43455_cfg.c` + `cyw43455_security.c` (new) |
| 6. Data path TX/RX | ⬜ not started | `cyw43455_cfg.c` + `cyw43455_sdpcm.c` |

### Step 1 — FWIL layer (`cyw43455_fwil.c`) ✅ DONE

`cyw_fil_iovar_data_get/set`, `cyw_fil_iovar_int_get/set`,
`cyw_fil_cmd_data_get/set` all implemented and tested. Runtime path
uses `ioctl_cv` (per-device taskqueue); boot-time path polls directly.
Reference: `/usr/src/sys/contrib/dev/broadcom/brcm80211/brcmfmac/fwil.c`.

### Step 2 — net80211 attach scaffolding (`cyw43455_cfg.c`) ✅ DONE

`ieee80211_ifattach` complete. VAP create/delete, `IC_STA | IC_WPA |
IC_WME` capability bits, `iv_newstate` hook, MAC from firmware. All
wireless-functional callbacks (`cyw_scan_start`, `cyw_scan_end`,
`cyw_newstate`, `cyw_transmit`, `cyw_parent`) exist as named stubs
with milestone markers.

### Step 3 — Event dispatcher (new `cyw43455_events.c`) ⬜

**Do this first** — steps 4 and 5 both depend on the event pipeline.

- New file `cyw43455_events.c`; wire into the `CYW_SDPCM_CHAN_EVENT`
  arm of `cyw_sdpcm_task` (`cyw43455_sdpcm.c:111`, currently discards).
- Parse the BCMETH/BRCMF event header (event code, status, ifidx, bsscfg).
  Register a code→handler table; add handlers as steps 4/5 land.
- At `cyw_cfg_attach` time, set the `event_msgs` bitmask IOVAR to
  subscribe to `E_ESCAN_RESULT`, `E_SET_SSID`, `E_AUTH`, `E_ASSOC`,
  `E_LINK`, `E_DEAUTH`, `E_DISASSOC`, `E_IF`.
- Initial handler: just `device_printf` the decoded event name/status.
  Verify by attempting an escan and watching the result event arrive.

Reference: Linux `brcmfmac/fweh.c` lines 1–150 (dispatcher), `fweh.h`
(E_* codes); FreeBSD-brcmfmac `src/events.c`.

### Step 4 — Escan (new `cyw43455_scan.c`) ⬜

Depends on step 3 (event pipeline).

- Fill `cyw_scan_start` in `cyw43455_cfg.c:151`:
  build `brcmf_escan_params_le`, issue `escan` IOVAR (action = START).
- Fill `cyw_scan_end` (abort): issue `escan` IOVAR (action = ABORT).
- Register `E_ESCAN_RESULT` handler in `cyw43455_events.c` (from step 3).
  Parse `brcmf_escan_result_le`, iterate BSS entries, synthesize a
  probe-response frame, feed `ieee80211_add_scan()`.  The D11N chanspec
  → IEEE channel conversion is in `freebsd-brcmfmac/src/scan.c`.

Verify: `ifconfig wlan0 scan && ifconfig wlan0 list scan` shows home AP.

Reference: Linux `cfg80211.c` lines 3500–3700 (escan callback);
FreeBSD-brcmfmac `src/scan.c` (closest net80211 template).

### Step 5 — Association + WPA2 (`cyw43455_cfg.c` + new `cyw43455_security.c`) ⬜

Depends on step 3 (event pipeline).

**`cyw_parent` (`cyw43455_cfg.c:140`)** — on IFF_UP: issue `WLC_UP`
(fills the stub marked "Milestone 2.4").

**`cyw_newstate` (`cyw43455_cfg.c:52`)** — on `IEEE80211_S_AUTH` entry:
1. `WLC_DOWN`
2. Security IOVARs (new `cyw43455_security.c`):
   `wsec=4` (AES), `wpa_auth=0x80` (WPA2_PSK), `wsec_pmk` (PSK).
   **Do NOT set `sup_wpa=1`** — returns BADARG on this firmware.
   **Do NOT set `wpa_auth` flag 0x8000** (PSK_SHA256 unsupported).
3. `WLC_SET_INFRA=1`, `WLC_SET_AUTH=0` (open), `WLC_SET_SSID`.
4. `WLC_UP`.

`E_LINK` (flags.LINK set) → drive VAP to `IEEE80211_S_RUN`.
`E_DEAUTH` / `E_DISASSOC` → drive VAP back to `IEEE80211_S_SCAN`.
State-machine transitions require process context — run via `sc->rx_tq`.

PSK sysctl: `hw.cyw43455.psk` (write-only). Supply via sysctl before
`ifconfig wlan0 ssid`.

Verify: `ifconfig wlan0` shows `state RUN`; `E_LINK` logged with LINK flag.

Reference: Linux `cfg80211.c:brcmf_cfg80211_connect`;
FreeBSD-brcmfmac `src/security.c` (iovar ordering).

### Step 6 — Data path TX/RX ⬜

Can start after step 5; plumbing is independent but there's no useful
test until the link is up.

**TX** (fill `cyw_transmit` at `cyw43455_cfg.c:130`):
- Strip mbuf chain → linear buffer.
- Build `[SDPCM hdr (chan=DATA) | BDC hdr | 802.3 frame]`.
- Credit-wait (same pattern as runtime path in `cyw_fil_txrx`).
- `cyw_f2_write_block(sc, frame, framelen)`.

**RX** (fill `CYW_SDPCM_CHAN_DATA` arm at `cyw43455_sdpcm.c:115`):
- Strip SDPCM + BDC headers.
- Build mbuf from payload.
- `ieee80211_input` / `ieee80211_input_mimo` via the VAP.

No glom (aggregation) initially — one frame per F2 transfer.
Honor SDPCM flow-control bits to avoid silent TX drops.

Verify: `dhclient wlan0` gets a lease; `ping -c 5 8.8.8.8` works.

Reference: Linux `proto.c` (BDC strip/add);
FreeBSD-brcmfmac `src/sdpcm.c` lines 150–350 (CHAN_DATA build/parse).

### Firmware init IOVARs

These are split across two call sites based on when the firmware accepts them.

**During attach (boot-time polling path, before `cyw_sdpcm_attach`)** — issued
in `cyw_attach()` while `sdpcm_running` is still false.  These work on the
pre-association firmware:

```
iovar "roam_off"   = 1        # disable firmware roaming
iovar "btc_mode"   = 0        # disable BT coexistence (causes FEM issues)
iovar "mpc"        = 0        # disable minimum power consumption
iovar "allmulti"   = 1        # accept all multicast
```

**After `WLC_UP`, before announcing the interface** — issued in Step 3
(`event_msgs`) and Step 5 (`pm`).  These require the radio to be up:

```
iovar "event_msgs" = mask     # step 3 — subscribe to events
iovar "pm"         = 0        # step 5 — disable power management after WLC_UP
```

> **`pm=0` is deferred to post-`WLC_UP`** (Step 5, `cyw_parent`).  The
> firmware rejects the `pm` IOVAR (cmd 263, status `0xffffffe9` = EINVAL) when
> the radio has not yet been brought up with `WLC_UP`.  Issuing it during
> attach produces a spurious warning and has no effect.  Move the
> `cyw_fil_iovar_int_set(sc, "pm", 0)` call into `cyw_parent` alongside the
> `WLC_UP` IOCTL, after the radio state machine is running.

### Bring-up verification order

1. `kldload cyw43455` — firmware boots, version printed, MAC shown.
2. `ifconfig wlan0 create wlandev cyw43455_0` — VAP created.
3. `ifconfig wlan0 up` — `WLC_UP`; event pipeline starts receiving.
4. `ifconfig wlan0 scan` — scan results include `VelcroDoggler`.
5. `sysctl hw.cyw43455.psk="$(cat .wifi | grep psk | cut -d= -f2)"` — set PSK.
6. `ifconfig wlan0 ssid VelcroDoggler` — assoc begins; events:
   `E_AUTH` → `E_ASSOC` → `E_SET_SSID` (success) → `E_LINK` (up).
7. `dhclient wlan0` — DHCP succeeds.
8. `ping -c 5 8.8.8.8` — **first WiFi packet is the milestone gate.**
9. `kldunload cyw43455 && kldload cyw43455` — confirm 5-cycle regression
   still passes after each step.

### Exit criteria

- WiFi scan shows nearby networks including `VelcroDoggler`.
- WPA2-PSK association succeeds and `ifconfig wlan0` shows `state RUN`.
- `dhclient wlan0` obtains an IP address.
- `ping` works reliably.
- 5-cycle `kldunload`/`kldload` regression still passes.

### Risk log — milestone 2

- **net80211 FullMAC impedance mismatch.** net80211 expects SoftMAC control
  (host does auth/assoc framing). FullMAC firmware does this internally.
  The reference driver works around this by intercepting `iv_newstate` and
  translating to firmware IOCTLs. Fork this logic carefully.
- **Chanspec format.** BCM43455 uses D11N chanspec (11-bit, 2-bit subband).
  Must convert correctly to/from IEEE channel numbers. Reference
  `scan.c` handles this.
- **SDPCM flow control stalls.** If firmware asserts flow control and the
  driver doesn't respect it, TX will fail silently. Mitigation: honor
  flow control bits in every SDPCM header; add a counter for flow
  control events.
- **PSK_SHA256 not supported.** BCM43455 firmware v7.45 does not support
  `wpa_auth` flag 0x8000 (PSK_SHA256). Do not set this flag.
- **sup_wpa IOVAR.** Setting `sup_wpa=1` (internal supplicant) returns
  BADARG on this firmware. Use host-managed WPA key exchange.
- **Regdomain channel pruning.** net80211 rebuilds the channel list via
  `setregdomain`. Set to `SKU_DEBUG` at attach to preserve all
  firmware-provided channels, matching the reference driver.
- **net80211 state machine re-entrancy.** `iv_newstate` may be called from
  process context or taskqueue context. All firmware IOVARs sleep; call
  them only from the `sc->rx_tq` taskqueue thread, never from the callout.

---

## Milestone 3 — Stability, performance, and SDIO interrupts

**Goal:** Replace polling with SDIO interrupt-driven RX. Achieve reliable
throughput suitable for daily use. Harden error handling.

### Step 1 — SDIO interrupt support

- Investigate BCM2712 SDHCI SDIO interrupt delivery (DATA1 line or
  out-of-band GPIO).
- If SDIO in-band interrupts work via MMCCAM:
  - Register interrupt handler via `sdio_register_intr()` or equivalent.
  - In handler: read SDIO interrupt pending register, call
    `cyw_sdpcm_rx()` for F2 data available.
  - Unmask SDIO interrupt in F0 CCCR.
- If in-band interrupts don't work:
  - Fall back to 1 ms polling (tighten from 50 ms callout).
  - Use `eth_cfg`-style interrupt status check to minimize MMIO overhead.

### Step 2 — TX performance

- Implement `buf_ring` for TX queueing (replace direct `if_transmit`).
- Batch TX: send multiple SDPCM data frames per F2 burst if firmware
  credits allow.
- Consider glom (frame aggregation on SDIO) if throughput is insufficient.

### Step 3 — Error recovery

- Watchdog timer: detect firmware hangs (no IOCTL response within 5 sec).
- On firmware hang: reset chip (WL_REG_ON toggle), re-download firmware,
  re-attach net80211. Log event.
- SDIO error handling: retry on CRC errors, bus reset on repeated failures.
- F2 overrun handling: drain and resync SDPCM sequence numbers.

### Step 4 — Power management

- Enable firmware power management (`pm` IOVAR = 2 for PM2).
- Support `WL_REG_ON` deassert on `kldunload` or system suspend.
- BT_DEV_WAKE / BT_HOST_WAKE handling (deferred to milestone 4).

### Step 5 — Validation

1. `iperf3 -c <peer>` — measure throughput. Target: 50+ Mbps TCP
   (realistic for single-stream 802.11ac on SDIO bus).
2. 24-hour `ping -i 1` stability test — zero lost packets.
3. Roaming test: move between APs, verify re-association.
4. Link flap: disable/enable radio, confirm clean recovery.
5. `kldunload`/`kldload` stress: 100 cycles without panic.
6. `netstat -ss` error counters remain zero under load.

### Exit criteria

- Interrupt-driven or tight-polling RX with < 5 ms latency.
- `iperf3` shows sustained throughput (target board-limited by SDIO bus).
- 24-hour stability with zero unrecovered errors.
- Power management reduces idle current (observable via `sysctl`).
- Module unload/reload 100x clean.

### Risk log — milestone 3

- **SDIO interrupt support on BCM2712.** FreeBSD's MMCCAM may not support
  SDIO interrupts on this controller. Mitigation: tight polling fallback.
- **Firmware crash recovery.** The firmware may crash on certain channels
  or under heavy load. Mitigation: watchdog + full reset path.
- **DMA coherency on SDIO.** SDIO is non-DMA (PIO via host controller),
  so cache coherency is not an issue, but SDHCI DMA (ADMA2) to host
  memory requires proper `bus_dmamap_sync`.

---

## Milestone 4 — Bluetooth HCI over UART

**Goal:** Bluetooth appears as an HCI device. `hccontrol` can send commands,
inquiry works, pairing is possible.

### Step 1 — UART discovery and patchram

- Walk FDT to find BT UART node (`uarta` with `brcm,bcm43438-bt`
  compatible or similar).
- Assert BT_REG_ON (GPIO 29) high, wait for BT boot.
- Open UART at 115200 baud (default CYW43455 BT baud rate).
- Download BT patchram (`BCM4345C0.hcd`) via vendor-specific HCI commands:
  - HCI_VSC_DOWNLOAD_MINIDRIVER (0xFC2E)
  - HCI_VSC_WRITE_RAM (0xFC4C) — send HCD records
  - HCI_VSC_LAUNCH_RAM (0xFC4E) — execute patched firmware
- After patchram: reset HCI, change baud to 3 Mbps via
  HCI_VSC_UPDATE_BAUDRATE (0xFC18).

### Step 2 — HCI transport attachment

- Attach as an HCI UART transport to FreeBSD's `ng_hci` or `hci_uart`
  framework.
- Implement H4 (standard HCI UART) framing: 1-byte packet indicator +
  HCI packet.
- Register with `ng_hci` node for upper-layer Bluetooth stack access.

### Step 3 — Sleep signaling

- BT_HOST_WAKE: chip → host interrupt (BT has data).
- BT_DEV_WAKE: host → chip (wake chip from sleep).
- If supported on Pi 5 GPIO routing, configure these pins.
- Otherwise: keep chip awake while BT is active.

### Step 4 — Validation

1. `hccontrol -n ubt0hci inquiry` — discover nearby BT devices.
2. `hccontrol read_local_name` — shows CYW43455 BT name.
3. BT coexistence: simultaneous WiFi `iperf3` + BT inquiry — no hangs,
   no significant WiFi throughput drop.
4. Audio: A2DP streaming (if FreeBSD Bluetooth audio stack supports it).

### Exit criteria

- `hccontrol` operational for inquiry and basic commands.
- Patchram download succeeds on every `kldload`.
- BT + WiFi coexistence stable.

### Risk log — milestone 4

- **FreeBSD BT stack maturity.** FreeBSD's Bluetooth stack (`ng_hci`,
  `ng_l2cap`, `ng_btsocket`) is functional but less tested than Linux's
  BlueZ. May hit edge cases.
- **UART access.** The BT UART may already be claimed by `uart(4)` in the
  base system. May need to prevent auto-attach or create a custom UART
  consumer.
- **Patchram format.** HCD file format is Broadcom-proprietary. Use the
  exact download sequence from Linux `btbcm.c` or the reference driver.
- **Coexistence firmware bug.** The reference driver notes that
  `btc_mode=1` causes FEM misconfiguration on CYW43455. Keep BT
  coexistence in mode 0 (firmware default GCI arbitration) unless
  testing proves otherwise.

---

## File summary

| File                  | Source                                    | Purpose                        |
|-----------------------|-------------------------------------------|--------------------------------|
| `cyw43455.c`          | New                                       | MOD_LOAD, FDT walk, power seq  |
| `cyw43455_sdio.c`     | Fork `brcmfmac/sdio.c`                   | SDIO F1/F2, backplane, clocks  |
| `cyw43455_sdpcm.c`    | Fork `brcmfmac/sdpcm.c`                  | SDPCM/BCDC framing, RX poll    |
| `cyw43455_fwil.c`     | Fork `brcmfmac/fwil.c`                   | IOCTL/IOVAR encoding           |
| `cyw43455_fw.c`       | Fork `brcmfmac/main.c` + `sdio.c`        | Firmware + NVRAM + CLM loading  |
| `cyw43455_cfg.c`      | Fork `brcmfmac/cfg.c`                    | net80211, VAP, link events     |
| `cyw43455_scan.c`     | Fork `brcmfmac/scan.c`                   | escan, chanspec, results       |
| `cyw43455_security.c` | Fork `brcmfmac/security.c`               | wsec, wpa_auth, keys, PSK      |
| `cyw43455_var.h`      | Fork `brcmfmac/brcmfmac.h`              | Softc, bus ops, SDPCM structs  |
| `cyw43455_cfg.h`      | Fork `brcmfmac/cfg.h`                    | Event codes, cfg structs       |
| `Makefile.cyw43455`   | New (pattern from `Makefile.rp1_eth`)     | Build configuration            |
| `tools/cyw43455_*.sh` | New                                       | Diagnostic scripts             |

## sysctl interface

**Runtime configuration (`hw.cyw43455.`):**

- `psk` — WPA passphrase (write-only, 8-63 chars)
- `pm` — Power management (0=off, 1=PM1, 2=PM2)
- `debug` — Debug verbosity (0=off, 1=info, 2=verbose)

**Read-only status:**

- `firmware_version` — Firmware version string
- `chip_id` — Chip ID (0x4345)
- `chip_rev` — Chip revision
- `mac_address` — Current MAC address
- `link.state` — Associated / scanning / idle
- `link.ssid` — Current SSID (if associated)
- `link.rssi` — Current RSSI (dBm)
- `link.channel` — Current channel number
- `sdio.f2_ready` — F2 function ready
- `sdio.tx_frames` — TX frame counter
- `sdio.rx_frames` — RX frame counter
- `sdio.flow_control_events` — Flow control assertion count
- `scan.results` — Number of cached scan results
- `scan.last_duration_ms` — Last scan duration
