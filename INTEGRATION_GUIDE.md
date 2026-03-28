# Raspberry Pi 5 Cooling Fan Control System for FreeBSD

## Architecture Overview

This is a complete kernel-based solution for controlling the Raspberry Pi 5 cooling fan on FreeBSD. It consists of three integrated components:

### 1. RP1 PWM Controller Driver (`rp1_pwm_driver.c`)

**Purpose**: Provides hardware access to the RP1 PWM controller on the Raspberry Pi 5.

**Key Features**:
- Registers with FreeBSD's `pwmbus` subsystem as a device driver
- Manages 4 PWM channels (the RP1 has 4 PWM outputs)
- Converts period/duty cycle specifications to hardware register values
- Implements polarity control (normal/inverted)
- Thread-safe register access via mutex locks

**Hardware Interface**:
```
RP1 Base Address:      0xe0000 (relative to RP1 MMIO space)
PWM Channels:          4 (indexed 0-3)
Clock Frequency:       125 MHz
Register Layout:       Per-channel offset of 0x2800 bytes
```

**Register Map**:
```
CSR (Control/Status):  0x00 - Enable, polarity, clock mode
DIV (Divider):         0x04 - Clock divider (1-256)
TOP (Period):          0x08 - PWM period (0-65535 counts)
CC (Compare):          0x0c - Duty cycle compare value
CTRA/B (A/B Compare):  0x10-0x14 - Channel A/B duty cycles
FRAC (Fractional):     0x18 - Fractional clock divider
```

**Device Tree Integration**:
The driver expects a device tree node:
```dts
rp1_pwm0: pwm@e0000 {
	compatible = "raspberrypi,rp1-pwm";
	reg = <0xe0000 0x2800>;
	#pwm-cells = <2>;
	status = "okay";
};
```

### 2. Thermal Management Module (`rpi5_cooling_fan_integrated.c`)

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

### Compile the RP1 PWM Driver

```bash
cd ~/rp1_pwm_driver
make
sudo make install
```

### Compile the Thermal Management Module

```bash
cd ~/rpi5_cooling_fan
make
sudo make install
```

## Device Tree Configuration

Add to your FreeBSD device tree (typically at `/usr/src/sys/arm64/broadcom/bcm2712-rpi-5-b.dts`):

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

Or load via device tree overlay at boot.

## Loading the Modules

### Manual Loading

```bash
# Load the PWM driver first
kldload rp1_pwm

# Then load the thermal management module
kldload rpi5_cooling_fan
```

### Automatic Boot Loading

Edit `/boot/loader.conf`:
```
rp1_pwm_load="YES"
rpi5_cooling_fan_load="YES"
```

Or `/etc/rc.conf`:
```
kld_list="rp1_pwm rpi5_cooling_fan"
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

The fan uses **PWM channel 3** (on RP1 pwmchip2):
- GPIO pin: 45
- Function: PWM0_CHAN3
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
[rp1_pwm_driver PWMbus Interface]
          ↓
[RP1 Hardware Registers]
          ↓
[PWM Output → Fan Motor]
```

## Troubleshooting

### PWM Driver Not Loading

```bash
# Check device tree compatibility
dmesg | grep rp1_pwm

# Verify device tree node exists
ls -la /proc/device-tree/RP1/pwm0/

# Check pwmbus is loaded
kldstat | grep pwmbus
```

**Solution**: Ensure device tree includes RP1 PWM node with correct compatible string.

### Fan Not Responding to Temperature Changes

```bash
# Verify sysctl values are readable
sysctl hw.rpi5.cooling_fan.cpu_temp
sysctl hw.rpi5.cooling_fan.current_state

# Check if PWM device was detected
dmesg | grep "RP1 PWM controller"
```

**Solution**: Manually specify PWM device during module initialization.

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
3. **Fan Tachometer Feedback**: Read RPM via pwmbus interface and implement PID control
4. **Thermal Event Logging**: Log fan state transitions and temperature spikes
5. **Adaptive Hysteresis**: Dynamically adjust hysteresis based on temperature rate of change
6. **GUI Dashboard**: sysutils port with FreeBSD-specific thermal monitoring

## References

- FreeBSD pwmbus(9) manual
- FreeBSD pwmc(4) manual
- Raspberry Pi 5 RP1 Register Specifications
- BCM2712 Thermal Sensor Documentation

## Support and Contributing

For issues or improvements:
1. Check FreeBSD kernel logs: `dmesg | tail -50`
2. Review sysctl output: `sysctl -a hw.rpi5.cooling_fan`
3. Test with manual PWM commands via pwm(8) utility
4. Consult FreeBSD device driver documentation

## License

BSD 2-Clause (SPDX-License-Identifier: BSD-2-Clause)
