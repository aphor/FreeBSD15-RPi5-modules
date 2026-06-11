# Raspberry Pi 5 Cooling Fan Control System for FreeBSD

## Architecture Overview

This is a complete kernel-based solution for controlling the Raspberry Pi 5 cooling fan on FreeBSD. It consists of three integrated components:

### 1. RP1 PWM Controller Driver (`bcm2712.c`)

**Purpose**: Provides hardware access to the RP1 PWM controller on the Raspberry Pi 5.

**Key Features**:
- Maps the RP1 PWM1 register window **directly by physical address** through
  the RP1 pcie2 outbound window (`pmap_mapdev_attr()`) — it does **not** register
  with the `pwmbus` subsystem and does **not** consume a device-tree PWM node
- Manages 4 PWM channels (the RP1 PWM block has 4 outputs)
- Converts period/duty cycle specifications to hardware register values
- Implements inverted polarity (required for the Pi 5 fan wiring)
- Enables the RP1 PWM1 clock (clock index 18) before use — the clock is gated
  off at reset, so writes appear to succeed but produce no output until enabled
- Thread-safe register access via mutex locks
- Driven by `rpi5.c`, which calls `bcm2712_pwm_set_config()` /
  `bcm2712_pwm_enable()` directly (no pwmbus plumbing)

**Hardware Interface**:
```
RP1 PWM1 CPU phys base: 0x1f0009c000  (RP1 base 0x1f_00000000 + offset 0x9c000)
                        reached via the pcie2 outbound window
PWM Channels:           4 (indexed 0-3); the fan uses channel 3
Source Clock:           50 MHz xosc → ÷8.138 → 6.144 MHz effective PWM clock
                        (Linux DTB: assigned-clock-rates = <6144000>)
Mapping Size:           0x1000
```

**Register Map** (RP1 layout, per `bcm2712_var.h` / Linux `pwm-rp1.c`):
```
GLOBAL_CTRL:   0x000          - per-channel enable bits + SET_UPDATE commit bit
CHAN_CTRL(x):  0x014 + x*16   - channel mode/polarity   (ch3 = 0x044)
RANGE(x):      0x018 + x*16   - channel period (ticks)  (ch3 = 0x048)
DUTY(x):       0x020 + x*16   - channel duty   (ticks)  (ch3 = 0x050)
```
Per-channel stride is 16 bytes (not a 0x2800 block).

**Device Tree Integration**:
The FreeBSD driver does **not** read a PWM node from the device tree — it maps
the RP1 PWM1 registers by physical address. For reference, the vendor Raspberry
Pi DTB (`bcm2712-rpi-5-b.dtb`) describes two PWM controllers under
`/axi/pcie@1000120000/rp1/`, both left **disabled** (FreeBSD does not use the
Linux `pwm-fan` path):
```dts
/* pwm0 — alias rp1_pwm0 */
pwm@98000 {
	compatible = "raspberrypi,rp1-pwm";
	reg = <0xc0 0x40098000 0x0 0x100>;
	#pwm-cells = <3>;
	status = "disabled";
};
/* pwm1 — alias rp1_pwm1; drives GPIO45 → fan (channel 3) */
pwm@9c000 {
	compatible = "raspberrypi,rp1-pwm";
	reg = <0xc0 0x4009c000 0x0 0x100>;
	#pwm-cells = <3>;
	status = "disabled";
};
```
No device-tree edit or overlay is required for the fan to work.

### 2. Thermal Management Module (`rpi5.c`)

**Purpose**: Monitors CPU temperature and controls fan speed based on configurable thresholds.

**Key Features**:
- Periodic temperature polling (every 1 second via callout)
- 4-level fan control with hysteresis
- Dynamic threshold and speed configuration via sysctl
- Persistent configuration via `/etc/sysctl.conf`
- Real-time thermal updates

**Thermal Control Logic**:

```
Temperature Range       Fan Level   PWM Duty (%)   Default Speed (0-255)
Below 50°C              Off         0%             0
50°C - 60°C             Level 1     ~29%           75
60°C - 67.5°C           Level 2     ~49%           125
67.5°C - 75°C           Level 3     ~69%           175
Above 75°C              Level 4     ~98%           250
```

**Hysteresis Implementation**:
Prevents rapid fan switching when temperature hovers near thresholds.
Default: 5°C hysteresis between each level.

Example: If fan is at Level 1 (enabled at 50°C), it won't drop back to Level 0 until temperature falls below 45°C (50 - 5).

### 3. System Integration Points

**sysctl Interface**:
```
hw.rpi5.cooling_fan.temp0         - Temperature threshold L0 (mC)
hw.rpi5.cooling_fan.temp1         - Temperature threshold L1 (mC)
hw.rpi5.cooling_fan.temp2         - Temperature threshold L2 (mC)
hw.rpi5.cooling_fan.temp3         - Temperature threshold L3 (mC)
hw.rpi5.cooling_fan.temp0_hyst    - Hysteresis L0 (mC)
hw.rpi5.cooling_fan.temp1_hyst    - Hysteresis L1 (mC)
hw.rpi5.cooling_fan.temp2_hyst    - Hysteresis L2 (mC)
hw.rpi5.cooling_fan.temp3_hyst    - Hysteresis L3 (mC)
hw.rpi5.cooling_fan.speed0        - PWM speed L0 (0-255)
hw.rpi5.cooling_fan.speed1        - PWM speed L1 (0-255)
hw.rpi5.cooling_fan.speed2        - PWM speed L2 (0-255)
hw.rpi5.cooling_fan.speed3        - PWM speed L3 (0-255)
hw.rpi5.cooling_fan.cpu_temp      - Current CPU temp (read-only, mC)
hw.rpi5.cooling_fan.current_state - Current fan level (read-only, 0-3)
```

## Compilation

Both modules are built from the consolidated Makefile at the repository root
(see `BUILDING.md`). `bcm2712` provides the PWM/thermal hardware access; `rpi5`
is the board thermal-management module and auto-loads `bcm2712`.

```bash
# Build the BCM2712 hardware module (RP1 PWM + thermal sensor)
make bcm2712

# Build the RPi5 board thermal-management module
make rpi5

# Or build everything and install
make
sudo make install
```

## Device Tree Configuration

**No device-tree change is required.** The `bcm2712` driver maps the RP1 PWM1
registers by physical address (`0x1f0009c000`) through the pcie2 outbound
window; it never probes a PWM node from the FDT, so neither the board `.dts` nor
an overlay needs editing for the fan to work.

For reference only, the vendor DTB shipped with the Pi 5
(`/boot/efi/efi/bcm2712-rpi-5-b.dtb`) already contains the RP1 PWM controllers
under `/axi/pcie@1000120000/rp1/`, but leaves both them and the `cooling_fan`
(`compatible = "pwm-fan"`) node `status = "disabled"` because FreeBSD does not
use the Linux `pwm-fan` driver:

```dts
pwm@9c000 {                                 /* rp1_pwm1 — fan controller */
	compatible = "raspberrypi,rp1-pwm";
	reg = <0xc0 0x4009c000 0x0 0x100>;
	#pwm-cells = <3>;
	status = "disabled";
};

cooling_fan {
	compatible = "pwm-fan";
	status = "disabled";
	cooling-levels = <0 75 125 175 250>;    /* matches default speeds below */
	pwms = <&rp1_pwm1 3 41566 1>;           /* channel 3, 41566 ns, inverted */
};
```

## Loading the Modules

### Manual Loading

```bash
# Loading rpi5 auto-loads its bcm2712 dependency (which owns the PWM hardware)
kldload rpi5

# (equivalently, load the hardware module explicitly first)
kldload bcm2712
kldload rpi5
```

### Automatic Boot Loading

Edit `/boot/loader.conf` (only `rpi5_load` is required — it pulls in bcm2712):
```
rpi5_load="YES"
```

Or `/etc/rc.conf`:
```
kld_list="bcm2712 rpi5"
```

## Configuration Examples

### View Current Configuration

```bash
sysctl -a hw.rpi5.cooling_fan
```

### Aggressive Cooling (Lower Thresholds, Higher Speeds)

```bash
sudo sysctl hw.rpi5.cooling_fan.temp0=40000
sudo sysctl hw.rpi5.cooling_fan.temp1=50000
sudo sysctl hw.rpi5.cooling_fan.temp2=60000
sudo sysctl hw.rpi5.cooling_fan.temp3=70000

sudo sysctl hw.rpi5.cooling_fan.speed0=150
sudo sysctl hw.rpi5.cooling_fan.speed1=180
sudo sysctl hw.rpi5.cooling_fan.speed2=220
sudo sysctl hw.rpi5.cooling_fan.speed3=255
```

### Quiet Cooling (Higher Thresholds, Lower Speeds)

```bash
sudo sysctl hw.rpi5.cooling_fan.temp0=60000
sudo sysctl hw.rpi5.cooling_fan.temp1=70000
sudo sysctl hw.rpi5.cooling_fan.temp2=80000
sudo sysctl hw.rpi5.cooling_fan.temp3=90000

sudo sysctl hw.rpi5.cooling_fan.speed0=50
sudo sysctl hw.rpi5.cooling_fan.speed1=100
sudo sysctl hw.rpi5.cooling_fan.speed2=150
sudo sysctl hw.rpi5.cooling_fan.speed3=255
```

### Make Changes Persistent

Edit `/etc/sysctl.conf`:
```
hw.rpi5.cooling_fan.temp0=45000
hw.rpi5.cooling_fan.speed0=100
hw.rpi5.cooling_fan.temp0_hyst=5000
```

Apply immediately:
```bash
sudo sysctl -f /etc/sysctl.conf
```

## Monitoring

### Real-time Monitoring

```bash
watch 'sysctl hw.rpi5.cooling_fan'
```

### Log Fan Events

Create a monitoring script:
```bash
#!/bin/sh
while true; do
    state=$(sysctl -n hw.rpi5.cooling_fan.current_state)
    temp=$(sysctl -n hw.rpi5.cooling_fan.cpu_temp)
    speed=$(sysctl -n hw.rpi5.cooling_fan.speed0)
    echo "$(date): State=$state Temp=$((temp/1000))°C Speed=$speed"
    sleep 1
done
```

## Hardware Integration Details

### CPU Temperature Source

The module currently reads temperature from a placeholder function. In production, integrate with:

1. **hwmon interface** (if available):
   ```c
   sysctl hw.sensors.cputemp0 (if coretemp driver present)
   ```

2. **Device tree thermal zone**:
   ```c
   Access via /dev/thermal or similar interface
   ```

3. **BCM2712 thermal sensor**:
   ```c
   Direct register access to SoC temperature sensor
   ```

### PWM Channel Selection

The fan uses **PWM channel 3** on RP1 PWM1 (`pwm@9c000`, alias `rp1_pwm1`):
- GPIO pin: 45 (ALT0 = pwm1)
- Period: 41566 ns (~24 kHz)
- Polarity: Inverted (0% duty = full voltage/off, 100% = 0V/on)

### Fan Control Flow

```
[CPU Temperature Sensor]
          ↓
[Thermal Polling Loop (1s interval)]
          ↓
[Threshold Comparison + Hysteresis]
          ↓
[PWM Speed Selection]
          ↓
[bcm2712_pwm_set_config() / bcm2712_pwm_enable()]
          ↓
[RP1 PWM1 Hardware Registers (direct MMIO map)]
          ↓
[PWM Output → Fan Motor]
```

## Troubleshooting

### PWM Driver Not Loading

```bash
# Verify the bcm2712 module (which owns the PWM mapping) is loaded
kldstat | grep -E "bcm2712|rpi5"

# Check the driver attached and mapped the PWM registers
dmesg | grep -iE "bcm2712|RP1 PWM"

# Inspect the live PWM channel-3 registers (read-only diagnostic sysctl)
sysctl hw.bcm2712.pwm_regs
```

**Solution**: Load `rpi5` (it auto-loads `bcm2712`). The driver maps RP1 PWM1 by
physical address, so no device-tree node or `compatible` string is involved.

### Fan Not Responding to Temperature Changes

```bash
# Verify sysctl values are readable
sysctl hw.rpi5.cooling_fan.cpu_temp
sysctl hw.rpi5.cooling_fan.current_state

# Inspect GLOBAL_CTRL / CHAN_CTRL3 / RANGE3 / DUTY3 directly
sysctl hw.bcm2712.pwm_regs
```

**Solution**: Confirm the RP1 PWM1 clock was enabled at attach (look for "RP1
PWM1 clock enabled" in `dmesg`); without it the channel output never changes.

### CPU Temperature Not Updating

The default implementation returns a placeholder value (50°C). 

**Fix**: Implement actual temperature reading:

```c
static int
rpi5_read_cpu_temp(uint32_t *temp)
{
	/* Option 1: Read from sysctl */
	size_t len = sizeof(int);
	int raw_temp;
	sysctlbyname("dev.cpu.0.temperature", &raw_temp, &len, NULL, 0);
	*temp = raw_temp * 1000;  /* Convert to mC */
	
	/* Option 2: Read from hwmon */
	FILE *fp = fopen("/sys/class/hwmon/hwmon0/temp1_input", "r");
	if (fp) {
		fscanf(fp, "%d", (int *)temp);
		fclose(fp);
	}
	
	return (0);
}
```

### High Fan Noise

Increase hysteresis to prevent rapid state changes:

```bash
sudo sysctl hw.rpi5.cooling_fan.temp0_hyst=10000  # 10°C instead of 5°C
```

## Performance Considerations

### Memory Footprint
- Driver: ~8 KB
- Module: ~12 KB
- Total: ~20 KB resident in kernel memory

### CPU Usage
- Temperature polling: <1% (runs every 1 second)
- PWM updates: Minimal (only on state changes)

### Response Time
- Temperature change to fan adjustment: ~1-2 seconds
- Limited by the 1-second polling interval

## Known Limitations

1. **CPU Temperature Reading**: Currently uses placeholder; needs integration with actual BCM2712 thermal sensor
2. **Single Fan Control**: Module controls one fan (channel 3); could be extended for multiple fans
3. **No ACPI/PCI Enumeration**: Requires manual device tree configuration; could be enhanced for auto-discovery
4. **Linux Compatibility**: Different from Linux pwmchip interface; FreeBSD-specific

## Future Enhancements

1. **Automatic Temperature Sensor Discovery**: Use devclass_find() to locate thermal sensors
2. **Multiple Fan Support**: Extend to control multiple independent fan zones
3. **Fan Tachometer Feedback**: Read RPM via the RP1 PWM TACH register
   (offset 0x3C, already exposed for diagnostics) and implement PID control
4. **Thermal Event Logging**: Log fan state transitions and temperature spikes
5. **Adaptive Hysteresis**: Dynamically adjust hysteresis based on temperature rate of change
6. **GUI Dashboard**: sysutils port with FreeBSD-specific thermal monitoring

## References

- Linux `pwm-rp1.c` (RP1 PWM register layout reference)
- Linux `clk-rp1.c` / `pinctrl-rp1.c` (PWM1 clock + GPIO45 ALT0 reference)
- Raspberry Pi RP1 Peripherals Specification (RP-008370-DS)
- Raspberry Pi 5 vendor DTB `bcm2712-rpi-5-b.dtb`
- BCM2712 AVS thermal sensor (register offset 0x200)

## Support and Contributing

For issues or improvements:
1. Check FreeBSD kernel logs: `dmesg | tail -50`
2. Review sysctl output: `sysctl -a hw.rpi5.cooling_fan`
3. Test with manual PWM commands via pwm(8) utility
4. Consult FreeBSD device driver documentation

## License

BSD 2-Clause (SPDX-License-Identifier: BSD-2-Clause)
