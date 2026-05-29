# Fan PWM Troubleshooting — Resolved

Status: **RESOLVED** (commit `27f384b`).  Root cause was B7 (PWM clock
disabled at reset) compounded by A1/A2 (duty not applied on first tick or on
same-state speed changes).  All three bugs were fixed and verified on hardware.

## Resolution summary

### Root cause: B7 — RP1 PWM1 clock disabled at reset

The RP1 SYS_CLOCKS controller leaves the PWM0/PWM1 clocks disabled after
reset.  Our code mapped the PWM registers and wrote RANGE/DUTY/GLOBAL_CTRL
successfully, but without a running clock the PWM peripheral never drove any
output — GPIO45 floated, the fan's pull-up held the line high, and the fan
free-ran in "no-signal safety mode" at ~9900 RPM regardless of duty setting.

Fix (`bcm2712.c`): map the RP1 clock controller at `0x1F00018000`, configure
clock 18 (PWM1) with source=50 MHz xosc and ÷8.138 divisor (→ 6.144 MHz),
then set ENABLE.  The 6.144 MHz clock was chosen by the Linux DTB
(`assigned-clock-rates = <6144000>`) so that the 24 kHz PWM period spans
exactly 255 ticks — making speed (0–255) map directly to duty ticks.

Clock register values used (verified working):
```
RP1_CLK_BASE_PHYS    = 0x1F00018000
PWM1 CTRL  (+0x84)   = 0x00000840   (ENABLE=bit11, AUXSRC=bit6)
PWM1 DIV_INT (+0x88) = 8
PWM1 DIV_FRAC (+0x8C)= 0x23550000  (→ 6.144 MHz)
```
After enabling, CTRL readback shows 0x10000840 — bit 28 (BUSY) confirms
the clock is running.

### Secondary bugs also fixed

**A2 — No initial PWM write**: the first 1 Hz callout saw
`new_state == current_state == 0` and skipped the PWM write, leaving the
peripheral un-programmed at boot.

**A1 — Same-state speed change**: writing `hw.rpi5.fan.speedN` while already
in state N had no effect until the next state transition.

Both fixed by applying PWM unconditionally on every callout tick (not only on
state transitions), and idling state 0 at `fan_temp0_speed` instead of 0.

### New diagnostic sysctl

`hw.bcm2712.pwm_regs` (read-only string) reads GLOBAL_CTRL, CHAN_CTRL3,
RANGE3, and DUTY3 from the already-mapped `sc->pwm_vaddr` — safe register
inspection without `/dev/mem` (which panics above the DMAP boundary at
`0x1F...` addresses).

### How the diagnosis unfolded

1. **Test 1** (force state transition): state transitioned 0→1 but RPM stayed
   flat.  This ruled out A1 alone as the cause and pointed to B or C.
2. **Boot dmesg**: confirmed pin mux code ran:
   `GPIO45 CTRL before=0x85 (FUNCSEL=5) → after=0x80 (FUNCSEL=0, pwm1)`.
   Ruled out C8 (no mux) and C9 (wrong pin — GPIO45 IS the fan PWM pin).
3. **`/dev/mem` attempt**: panicked (`pmap_map_io_transient: TODO: Map out of
   DMAP data`) — RP1 addresses at `0x1F...` are above the DMAP boundary.
4. **Web research**: RPi forum confirmed RP1 PWM clocks are disabled by
   default.  Exact clock register addresses and values from community findings.
5. **Fix + test**: after enabling the clock, DUTY3 tracked speed0 exactly
   across the full 0–250 sweep, and same-state speed changes took effect
   within one second — all without the `/dev/mem` approach.

---

## Original investigation (preserved below)

## Initial assessment (data)

Tachometer (`hw.rpi5.fan.rpm`) is treated as ground truth: validated by
manually slowing the fan rotor and observing RPM drop in real time.

CPU was idle at 38.6 °C, well below `temp0` (50 °C), so the controller
stays in `current_state = 0`. With `current_state` constant we changed
`hw.rpi5.fan.speed0` from 0 to 250 in steps and recorded RPM after a
3-second settle:

| `speed0` (PWM 0–255) | Expected duty | `current_state` | Measured RPM |
|---:|---:|:---:|---:|
|   0 |   0 % | 0 | 9871 |
|  50 |  20 % | 0 | 9868 |
| 100 |  39 % | 0 | 9874 |
| 150 |  59 % | 0 | 9894 |
| 200 |  78 % | 0 | 9897 |
| 250 |  98 % | 0 | 9897 |

**RPM range across full PWM sweep: 9868 – 9897 (Δ = 0.3 %).**

Result: **PWM duty changes have no measurable effect on fan speed.**
The fan is locked at ~9900 RPM regardless of setting. Either
(a) the PWM signal is not reaching the fan PWM input, or
(b) it is reaching the fan but always represents the same effective
duty (e.g. stuck at 0 % "off-inverted" = 100 % on).

## Field of possible causes

Bucketed from "closest to userland" outward to hardware:

### A. Controller / dispatch logic (rpi5.c)
1. `current_state` only triggers a `pwm_channel_config()` call on a
   *state transition*. Writing `speed0` while `current_state` already
   equals 0 may not re-apply duty.
2. The 1 Hz callout reads the active speed from the wrong array index
   (off-by-one between `current_state` and `speed[]`).
3. `hw.rpi5.fan.speed0` writes update only the in-memory variable, not
   the live PWM channel.

### B. PWM peripheral programming (bcm2712.c)
4. `pwm_channel_config()` is called but the period/duty arguments are
   computed wrong (e.g. duty in ns vs. raw counts swapped, or period
   = 0 falling back to 100 % on).
5. The polarity bit is inverted from what the fan expects (we set
   "inverted" but the fan PWM input is direct, or vice versa).
6. The PWM channel index is wrong — the real fan-driving channel is
   not `pwmc?.3` but a different one; we are programming an unused
   channel while the fan PWM input is held by another, untouched
   channel running at 100 % from boot.
7. The PWM clock or enable bit is never set, so register writes appear
   to succeed but the peripheral output is permanently driving the
   default level.

### C. Pin mux / signal routing (rp1_gpio M1, firmware)
8. The GPIO pin that drives the fan PWM line is **not** muxed to the
   PWM alternate function — it is a plain GPIO. With our M1 driver
   (no pinctrl yet) we rely on whatever firmware left in the FUNCSEL
   register at boot. If firmware did not set FUNCSEL = PWM, our PWM
   peripheral output never reaches the connector.
9. The pin we *think* is FAN_PWM (e.g. GPIO 44) is not actually the
   one wired to the fan header; the schematic uses a different pin.

### D. Fan / hardware
10. The Pi 5 active cooler is wired such that the PWM input is pulled
    high by a default resistor and the fan free-runs at maximum
    whenever PWM is absent. Combined with any of A/B/C, this
    perfectly explains a flat ~9900 RPM.
11. Power cycle / boot leaves the fan controller in a "panic-on" state
    that only a real PWM edge can reset.

## Bifurcation strategy

Goal: with the fewest tests, isolate which of A / B / C / D is the
cause. Each test cuts the search space roughly in half.

### Test 1 — Force a state transition

```sh
sysctl hw.rpi5.fan.temp0=10000     # 10 °C — guaranteed below CPU idle
sleep 3
sysctl hw.rpi5.fan.current_state hw.rpi5.fan.rpm
sysctl hw.rpi5.fan.speed0=0        # request fan off in the new state
sleep 3
sysctl hw.rpi5.fan.rpm
sysctl hw.rpi5.fan.temp0=50000     # restore
```

- **RPM responds** → cause is (A1) "duty not re-applied on speed change
  while in same state". Easy fix in rpi5.c: call the apply function
  from the speed sysctl handler.
- **RPM still flat** → A1 is eliminated. Move to Test 2.

### Test 2 — Read PWM peripheral state directly

Dump RP1 PWM control / duty / period registers via `devctl get` or a
small `dd if=/dev/mem` reader (or add a temporary `hw.bcm2712.pwm.*`
sysctl). Compare register contents at duty=0 vs. duty=250.

- **Registers don't change between settings** → cause is (A) — the
  driver isn't writing the hardware. Localize to `pwm_channel_config`
  call site.
- **Registers change correctly but RPM doesn't** → driver is doing its
  job; signal is not reaching fan. Move to Test 3.

### Test 3 — Inspect FUNCSEL on candidate fan-PWM pin

Use `tools/rp1_gpio_dump.sh <pin>` for the suspect pins (44, 45, 13,
12 — the four candidates per RP1 datasheet for `pwmc.3`). Check
which pin's FUNCSEL is currently set to the PWM alternate.

- **No pin is muxed to PWM** → cause (C8). Workaround: set the FUNCSEL
  manually (write to RP1 GPIO regs from a small script) and re-run
  Test 1. Long-term fix: rp1_gpio M2 (pinctrl).
- **A pin is muxed to PWM but it's not the fan's pin** → cause (C9 /
  B6). Check Pi 5 schematic / FDT `fan` node to find the real pin,
  then either change PWM channel selection in bcm2712.c or change the
  pinmux.
- **The "right" pin is muxed to PWM and registers are toggling** →
  cause is in zone (D). Move to Test 4.

### Test 4 — Override the fan-PWM pin as a plain GPIO

If we can prove which pin drives the fan, drive it manually:

```sh
gpioctl -c -f /dev/gpioc0 <pin> OUT       # claim as output
gpioctl -f /dev/gpioc0 <pin> 0             # drive low
sleep 3 ; sysctl -n hw.rpi5.fan.rpm
gpioctl -f /dev/gpioc0 <pin> 1             # drive high
sleep 3 ; sysctl -n hw.rpi5.fan.rpm
```

Whichever level produces the lower RPM tells us the fan's true PWM
polarity. If neither level produces an RPM change, the fan is not
actually responsive to that pin → re-evaluate which pin is FAN_PWM
(C9). If we *do* see RPM change here but not via the bcm2712 PWM
driver, the bug is firmly in B (PWM peripheral programming or
channel selection).

## Decision tree

```
                  Test 1 — speed change after forced state transition
                           |
              ┌────────────┴────────────┐
        RPM changes              RPM still flat
              |                          |
         (A1) — speed                  Test 2 — PWM registers vary?
         not re-applied                       |
         on same-state            ┌──────────┴──────────┐
         write                  No                     Yes
                                |                       |
                          (A/B-driver) —          Test 3 — pin FUNCSEL
                          isolate to               for candidate pins
                          register write site            |
                                                ┌────────┼────────┐
                                            no PWM  PWM pin     PWM=fan pin
                                            mux      ≠ fan pin   & toggling
                                              |        |             |
                                            (C8)     (B6/C9)       Test 4
                                                                     |
                                                              ┌──────┴──────┐
                                                          RPM moves    RPM flat
                                                              |          |
                                                          (B5 polarity (D — wiring
                                                          or B4 duty   assumption
                                                          calc)        wrong)
```

## Notes & risks

- All tests are non-destructive at runtime; nothing requires reboot.
- Test 4 requires a confident pin identification; mis-driving an SPI
  or I²C-shared line could disturb other subsystems. Before claiming
  a pin, capture the current FUNCSEL/PADS state with
  `tools/rp1_gpio_dump.sh` so we can restore it.
- Cause (A1) is by far the most likely given symptoms; the speed
  sysctl handler in `rpi5.c` should be inspected as a first read
  alongside running Test 1.
- This whole investigation should be doable in 30–45 minutes once
  approval is given to start running the tests.

## Source files to read first

- `rpi5.c` — sysctl handler for `speed{0..3}`, the 1 Hz callout, the
  `pwm_channel_config` call site
- `bcm2712.c` — PWM channel programming, period/duty register writes,
  polarity / enable bits
- `rp1_gpio.c` (M1) — confirm we are *not* yet touching FUNCSEL
- Pi 5 schematic / FDT `cooling-fan` node — to confirm the actual
  GPIO line driving the fan PWM input
