# Raspberry Pi 5 FreeBSD Kernel Modules

A set of FreeBSD kernel modules for the Raspberry Pi 5 (BCM2712 SoC + RP1
peripheral chip). Targets FreeBSD 16-CURRENT on arm64.

## Modules

| Module          | Status    | Purpose                                                        | Depends on  |
|-----------------|-----------|----------------------------------------------------------------|-------------|
| `bcm2712`       | ✓ working | BCM2712 thermal sensor and RP1 PWM controller (pwmbus)         | —           |
| `rpi5`          | ✓ working | Pi 5 cooling-fan thermal management (4-level PWM control)      | `bcm2712`   |
| `rp1_gpio`      | ✓ M1      | RP1 GPIO / Pinctrl controller �� 54 pins via `gpio_if(9)`       | —           |
| `rp1_eth`       | ⚙ M1 built | Cadence GEM Ethernet driver for the Pi 5 NIC (via RP1 PCIe2) | `bcm2712_pcie` (M3) |
| `bcm2712_pcie`  | ✓ working | ACPI shim that routes shared GIC SPI 229 to `rp1_eth` (M3)    | ACPI        |

`rpi5` auto-loads `bcm2712` via `MODULE_DEPEND`; `rp1_eth` will auto-load
`bcm2712_pcie` when M3 is complete.

## Building

Prerequisites:
- FreeBSD kernel sources at `/usr/src/sys` (the build pulls interface
  headers like `device_if.h`, `bus_if.h`, `pwmbus_if.h`, `acpi_if.h`
  directly from system `.m` files).
- A working `make` and toolchain (base FreeBSD).

```sh
make                          # build all modules
make bcm2712                  # build single module
make rpi5
make rp1_gpio
make rp1_eth                  # builds rp1_eth_cfg.c + rp1_eth.c → rp1_eth.ko
make bcm2712_pcie

sudo make install             # install all .ko files into /boot/modules
sudo make install-rpi5        # individual installs also available
sudo make install-rp1_gpio
```

`BUILDING.md` has the full reference for build flags and per-module
Makefiles.

## Loading

```sh
sudo make load                    # all modules
sudo kldload rpi5                 # auto-loads bcm2712
sudo kldload /boot/modules/rp1_gpio.ko   # GPIO / pinctrl controller

make status                       # show kldstat + sysctl summary
sudo make unload                  # rpi5 first, then bcm2712
```

> **Note:** `rp1_gpio` is installed to `/boot/modules/` (not `/boot/kernel/`),
> so use the full path with `kldload`.

To auto-load at boot, add to `/boot/loader.conf`:

```
rpi5_load="YES"           # auto-loads bcm2712
rp1_gpio_load="YES"
```

## Sysctl Interface

### `hw.bcm2712.*` — common BCM2712 hardware

| OID                            | Type      | Description                                        |
|--------------------------------|-----------|----------------------------------------------------|
| `hw.bcm2712.debug`             | RW int    | Verbose debug logging (0 = off, 1 = on)            |
| `hw.bcm2712.thermal.cpu_temp`  | RD int    | CPU temperature — stored as deciKelvin, displayed by `sysctl(8)` as Celsius (e.g. `47.9C`) |

### `hw.rpi5.fan.*` — cooling-fan thermal management

| OID                              | Type      | Default | Description                            |
|----------------------------------|-----------|---------|----------------------------------------|
| `hw.rpi5.fan.temp0`              | RW uint   | 50000   | Level 0 threshold (milli-°C = 50.000 °C) |
| `hw.rpi5.fan.temp1`              | RW uint   | 60000   | Level 1 threshold                      |
| `hw.rpi5.fan.temp2`              | RW uint   | 67500   | Level 2 threshold                      |
| `hw.rpi5.fan.temp3`              | RW uint   | 75000   | Level 3 threshold                      |
| `hw.rpi5.fan.temp{0..3}_hyst`    | RW uint   | 5000    | Per-level hysteresis (milli-°C)        |
| `hw.rpi5.fan.speed0`             | RW uint   | 75      | Level 0 PWM duty (0–255 �� 0–100 %)    |
| `hw.rpi5.fan.speed1`             | RW uint   | 125     | Level 1 PWM duty                       |
| `hw.rpi5.fan.speed2`             | RW uint   | 175     | Level 2 PWM duty                       |
| `hw.rpi5.fan.speed3`             | RW uint   | 250     | Level 3 PWM duty                       |
| `hw.rpi5.fan.cpu_temp`           | RD uint   | —       | Latest sampled CPU temperature (milli-°C) |
| `hw.rpi5.fan.current_state`      | RD uint   | —       | Active fan level (0–3)                 |
| `hw.rpi5.fan.rpm`                | RD uint   | —       | Fan tachometer reading (RPM)           |

The `rpi5` module polls `cpu_temp` once per second (1 Hz callout) and
selects a level using the thresholds and per-level hysteresis. The
selected PWM duty is driven through pwmbus channel 3 on the RP1.

| Level | Activates when CPU ≥ | Default PWM | Approx. duty |
|------:|----------------------|-------------|--------------|
| 0     | (always-on idle)     | 75          | 29 %         |
| 1     | 50 °C                | 125         | 49 %         |
| 2     | 60 °C                | 175         | 69 %         |
| 3     | 75 °C                | 250         | 98 %         |

The fan runs at the level-0 idle speed (PWM 75) even below the temp0
threshold. Use `temp0` / `speed0` to set the baseline idle duty.

Persist changes in `/etc/sysctl.conf`, e.g.:
```
hw.rpi5.fan.temp0=45000
hw.rpi5.fan.speed0=90
```

### `hw.rp1_eth.*` — Ethernet driver (M1 built; M2/M3 pending)

> `rp1_eth` is built but not loaded by default. Load it manually with
> `kldload /boot/modules/rp1_eth.ko` once M2 is ready.

| Subtree                  | Contents                                                           |
|--------------------------|--------------------------------------------------------------------|
| `hw.rp1_eth.mac_addr.*`  | OTP-derived MAC address (string + raw uints)                       |
| `hw.rp1_eth.cfg.*`       | `eth_cfg` PHY-glue register window (status, GPIO state, RD-only)   |
| `hw.rp1_eth.gem.*`       | GEM core register snapshot (diagnostic, RD-only)                   |
| `hw.rp1_eth.mac_drv.rxbufs` | RW int — RX descriptor ring size                                |
| `hw.rp1_eth.mac_drv._rxoverruns`, `_rxnobufs`, `_rxdmamapfails`, `_txfull`, `_txdmamapfails`, `_txdefrags` | RD counters — driver-side error tallies |
| `hw.rp1_eth.mac_drv.stats.tx_{bytes,frames,under_runs}` | RD — GEM hardware TX counters                |
| `hw.rp1_eth.mac_drv.stats.rx_{bytes,frames,frames_fcs_errs,overrun_errs,…}` | RD — GEM hardware RX counters |

The driver also creates an `if_t` named **`rp1eth0`** that participates
normally in `ifconfig(8)`, the routing table, BPF, and netgraph.

## GPIO (`rp1_gpio`) — Milestone 1

The `rp1_gpio` module attaches the RP1's GPIO / Pinctrl controller to the
FreeBSD `gpio_if(9)` and `gpiobus(4)` framework. It locates the hardware via
an FDT walk (no ACPI node required) and maps the three register banks via
`pmap_mapdev_attr`.

```
rp1_gpio0: <RP1 GPIO / Pinctrl Controller>
rp1_gpio0: IO_BANK@... RIO@... PADS@... (IRQ chain: deferred to M4)
gpiobus0: <GPIO bus> on rp1_gpio0
gpioc0: <GPIO controller> at pins 0-53 on gpiobus0
```

After loading, use `gpioctl(8)` with the `/dev/gpioc0` device:

```sh
gpioctl -lv /dev/gpioc0          # list all 54 pins with names and levels
gpioctl -f /dev/gpioc0 45        # read GPIO 45 level (0 or 1)
gpioctl -f /dev/gpioc0 45 1      # drive GPIO 45 high
```

Pin 45 maps to bank 2, index 11 (`FAN_TACH` on the standard Pi 5 board).
Symbolic names are populated from the FDT `gpio-line-names` property.

M1 does not expose a sysctl tree or handle interrupts. Those are scheduled
for M2 (pinctrl function-select) and M4 (per-pin edge/level IRQs).

## RP1 Ethernet (`rp1_eth`) — Milestones

`rp1_eth` is a fork of `sys/dev/cadence/if_cgem.c` adapted for the Pi 5
GEM_GXL MAC behind the RP1 PCIe2 outbound window. Development is split
into three milestones (see `if_gem-PLAN.md`):

- **M1 — `rp1_eth_cfg.c`**: FDT walk, map `eth_cfg` (PHY clock-mux /
  reset glue), drive PHY reset GPIO, expose `hw.rp1_eth.cfg.*` sysctls
  and a link-state observation tick. No network attach. *(built)*
- **M2 — polled mode**: forks `if_cgem`, attaches `rp1eth0` to the
  network stack, RX/TX driven by a 200 Hz callout (no interrupts).
- **M3 — interrupt-driven**: GIC SPI 229 (shared with `xhci0/1`) is
  routed through a small ACPI shim (`bcm2712_pcie`) that filter-checks
  `CGEM_INT_STATUS` and forwards to `cgem_intr_filter`. The filter
  defers RX/TX work to a `taskqueue_fast` swi task.

The shim publishes a tiny KPI (`bcm2712_pcie.h`):

```c
void bcm2712_pcie_register_rp1_intr(driver_filter_t *filter, void *arg);
void bcm2712_pcie_deregister_rp1_intr(void);
```

`child_filter` / `child_arg` are stored as `volatile uintptr_t` and
updated via `atomic_store_rel_ptr` / `atomic_load_acq_ptr` so the
filter never has to take a spin lock — important because FreeBSD's
arm64 interrupt dispatch already nests filter handlers inside an
implicit critical section.

### M3 ACPI / DSDT requirement

`bcm2712_pcie` matches an ACPI `_HID` of `"BCM2712"` placed inside the
RP1B scope. The default Pi 5 DSDT does not declare it; an override
(`/boot/acpi_dsdt.aml`) supplies the device with two MMIO resources
(GEM MAC + `eth_cfg`) and the shared interrupt (GSI 261 / GIC SPI 229).
Without the override M3 will not attach.

## Diagnostic Tools

Repeatable diagnostics live in `tools/` as shell scripts so each one
can be granted execute permission once and reused:

```sh
# rp1_gpio
sh tools/rp1_gpio_dump.sh 45        # dump CTRL/STATUS/PADS regs for GPIO 45

# rp1_eth
sh tools/rp1_eth_status.sh           # snapshot of module state + cfg regs
sh tools/rp1_eth_link_watch.sh       # live link-state monitor (Ctrl-C to stop)
sudo sh tools/rp1_eth_fdt_dump.sh    # dump FDT ethernet/phy/gpio nodes
sudo sh tools/rp1_eth_load.sh        # build + install + load + status
sudo sh tools/rp1_eth_load.sh --reload   # unload + reload without rebuild
sudo sh tools/rp1_eth_load.sh --unload   # unload only
```

The fan-control wrapper script:

```sh
chmod +x rpi5_fan_control_integrated.sh
./rpi5_fan_control_integrated.sh --check          # module status
./rpi5_fan_control_integrated.sh --diagnostics    # full diagnostics
./rpi5_fan_control_integrated.sh --monitor        # real-time temperature/fan
./rpi5_fan_control_integrated.sh --test           # PWM hardware exercise
```

## Device Tree Requirements

`rp1_gpio` and `rp1_eth_cfg` locate their hardware by walking the FDT
exported via `/dev/openfirm`; no matching ACPI node is required. The
system boots with ACPI but the Pi 5 firmware also publishes a full FDT,
which these drivers consume directly.

The PWM channel used by the fan controller requires:

```dts
&RP1 {
    pwm0: pwm@e0000 {
        compatible = "raspberrypi,rp1-pwm";
        reg = <0xe0000 0x2800>;
        #pwm-cells = <2>;
        status = "okay";
    };
};
```

`rp1_eth_cfg` walks `/soc/...` for `cdns,pi5-gem` (or compatible) and
its associated PHY / `eth_cfg` glue. M3 additionally requires the ACPI
DSDT override described above.

## Files of Interest

- `bcm2712.c`, `bcm2712_var.h` — BCM2712 thermal + RP1 PWM (pwmbus)
- `rpi5.c` — Pi 5 fan controller, `hw.rpi5.fan.*` sysctls
- `rp1_gpio.c`, `rp1_gpio_var.h` — RP1 GPIO / Pinctrl M1
- `rp1_eth.c`, `rp1_eth_cfg.c`, `rp1_eth_var.h`, `rp1_eth_hw.h`
- `bcm2712_pcie.c`, `bcm2712_pcie.h` — M3 interrupt router
- `Makefile` — top-level build, plus `Makefile.<module>` per KMOD
- `if_gem-PLAN.md` — design and milestone notes for the Ethernet driver
- `INTEGRATION_GUIDE.md` — architecture and integration details
- `BUILDING.md` — detailed build/install reference
- `tools/` — diagnostic shell scripts (see above)

## License

BSD 2-Clause (`SPDX-License-Identifier: BSD-2-Clause`).
