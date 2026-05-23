# brcmfmac vs. cyw43455 — Logical Design Differences

Comparison of the upstream FreeBSD `brcmfmac` reference driver
(`../freebsd-brcmfmac.git/src/`) against the forked `cyw43455` KLD. Both are
single-KLD, monolithic FullMAC drivers; the differences below are structural
choices made when narrowing the fork to one chip on one bus.

## 1. Bus scope and abstraction

| | brcmfmac | cyw43455 |
|---|---|---|
| Buses | PCIe **and** SDIO | SDIO only |
| Abstraction | `struct brcmf_bus_ops` vtable (`brcmfmac.h:154`) — `ioctl/tx/flowring_*/cleanup` | none — direct `sdio_read/write` calls |
| Instances | `brcmf_pcie_bus_ops` (`pcie.c:28`), `brcmf_sdio_bus_ops` (`sdpcm.c:836`) | n/a |

brcmfmac keeps PCIe (`pcie.c`, `msgbuf.c`) and SDIO paths behind a function-
pointer vtable so `fwil.c` is bus-agnostic (`sc->bus_ops->ioctl()`). cyw43455
deleted all PCIe/msgbuf code and calls SDIO F1/F2 directly, so the vtable is
gone — FWIL is hardwired to SDPCM/BCDC.

## 2. Protocol layer

brcmfmac supports two firmware protocols — ring-based **msgbuf** (PCIe,
`msgbuf.c`) and **SDPCM+BCDC** (SDIO, `sdpcm.c`) — both hidden behind
`bus_ops`. cyw43455 keeps only SDPCM/BCDC, and inlines the wire format
directly into `cyw43455_sdpcm.c` and `cyw43455_fwil.c` with no `proto`
indirection.

## 3. Chip / core enumeration

| | brcmfmac | cyw43455 |
|---|---|---|
| Location | dedicated `core.c` | folded into `cyw43455_sdio.c:373` |
| Scope | full EROM/DMP scan: ARM, D11, PCIE, SDIO, SOCRAM cores | EROM scan for SDIOD core; hardcoded fallback addresses in `cyw43455_var.h` |

brcmfmac treats chip enumeration as its own layer reused by both buses.
cyw43455 keeps only what the SDIO path needs and tolerates static addresses.

## 4. Firmware loading

The sharpest structural difference: brcmfmac calls `firmware_get()` from
within the driver (`pcie.c`/`sdio.c`). cyw43455 splits firmware out into a
**separate registrar KLD** — `cyw43455fw.ko` (`cyw43455_fwmod.c`) — whose only
job is to register `brcmfmac43455-sdio.{bin,txt}` via `firmware(9)` at
`MOD_LOAD`. `cyw43455_fw.c` then consumes them. This decouples firmware
packaging from driver code.

## 5. FWIL transaction model

brcmfmac: one path — sleep on an `ioctl_completed` flag, woken by the D2H
control response. cyw43455: explicit **two-path** design (`cyw43455_fwil.c`) —
boot-time **synchronous polling** (`cyw_sdpcm_recv_one`, before the RX
taskqueue exists) vs. runtime **condvar** (`ioctl_cv`) signalled by the RX
task. Necessary because cyw43455 issues IOVARs during attach before the
taskqueue is running.

## 6. Threading model

| | brcmfmac | cyw43455 |
|---|---|---|
| SDIO RX | 50 ms callout → `sdpcm_tq` (1 thread) | 50 ms callout → per-device `sc->rx_tq` |
| Link/scan events | global `taskqueue_thread` | same per-device `sc->rx_tq` |

brcmfmac offloads sleeping link/scan work to the global `taskqueue_thread`.
cyw43455 **must not** — `sdiob` enqueues device discovery on that thread, so
`cv_timedwait` in the FWIL path self-starves (documented in TODO.md M1
lessons). cyw43455 routes everything through one per-device taskqueue.

## 7. Event dispatch

brcmfmac dispatches firmware events inline in `brcmf_sdpcm_process_event()` /
`brcmf_msgbuf_process_d2h()` with a hardcoded switch. cyw43455
(`cyw43455_events.c`) adds a **128-entry handler table** with
`cyw_event_register/unregister()` — a more modular registration API that lets
each Milestone 2 step install its own per-code handler.

## 8. Softc structure

`brcmf_softc` (`brcmfmac.h:164`) is one monolithic context carrying *both*
PCIe DMA rings (`commonrings[5]`, DMA buffers) and SDIO state. `cyw_softc`
(`cyw43455_var.h:364`) is SDIO-only and leaner: backplane window, chip
identity, SDPCM seq/credits, the two-path IOCTL sync fields, `rx_tq`, and the
event table. Neither uses separate `brcmf_pub`/`brcmf_if` structs; both fold
everything into the softc and assume a single VAP.

## 9. Naming

All `brcmf_*` / `BRCMF_*` symbols renamed to `cyw_*` / `CYW_*` to avoid
symbol collisions if the real `brcmfmac` is ever loaded alongside
(design decision #1). Register-offset macros (`SBSDIO_*`, `BCMA_*`) keep
their upstream names.

## 10. Completeness (status, not design)

brcmfmac is a complete working driver. cyw43455's `cyw43455_cfg.c` is still
scaffolding — `cyw_scan_start`, `cyw_newstate`, `cyw_transmit` are named stubs
(Milestone 2 steps 4–6 unstarted). The bring-up layers (SDIO, SDPCM, FWIL,
firmware) are complete and regression-tested.

## Summary

cyw43455 is brcmfmac with the PCIe half amputated and the remaining SDIO half
de-abstracted: the `bus_ops` vtable and `proto` indirection are gone because
there is only one bus and one protocol. The deliberate *additions* over a
straight fork are: a separate firmware registrar KLD, a per-device taskqueue
(to dodge the `taskqueue_thread` deadlock), a two-path FWIL, and a
registration-table event dispatcher.
