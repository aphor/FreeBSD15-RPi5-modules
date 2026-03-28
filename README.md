# Raspberry Pi 5 Cooling Fan Sysctl Module for FreeBSD 15

A kernel module that exposes Raspberry Pi 5 cooling fan control variables as sysctl OIDs under the `hw.rpi5` tree.

## Features

- Read/write access to temperature thresholds (fan_temp0-3)
- Read/write access to hysteresis values (fan_temp0_hyst-3_hyst)
- Read/write access to PWM speed levels (speed0-3)
- Read-only access to current fan state
- Thread-safe access using mutex locks
- Input validation for all parameters

## Building

### Prerequisites
- FreeBSD 15 kernel sources
- Kernel module development tools

### Build Steps

```bash
# Clone or copy the module files to a build directory
mkdir -p ~/rpi5_cooling_fan
cd ~/rpi5_cooling_fan
cp rpi5_cooling_fan.c .
cp Makefile .

# Build the kernel module
make

# Install the module (requires root)
sudo make install
```

## Loading the Module

### Manual Loading
```bash
# Load the module
kldload rpi5_cooling_fan

# Verify it's loaded
kldstat | grep rpi5_cooling_fan
```

### Automatic Loading at Boot

Add to `/etc/rc.conf`:
```
kld_list="rpi5_cooling_fan"
```

Or add to `/boot/loader.conf`:
```
rpi5_cooling_fan_load="YES"
```

## Sysctl OIDs

All values are under `hw.rpi5.cooling_fan.*`

### Temperature Thresholds (milli-degrees Celsius)
- `hw.rpi5.cooling_fan.temp0` - Level 0 threshold (default: 50000 = 50°C)
- `hw.rpi5.cooling_fan.temp1` - Level 1 threshold (default: 60000 = 60°C)
- `hw.rpi5.cooling_fan.temp2` - Level 2 threshold (default: 67500 = 67.5°C)
- `hw.rpi5.cooling_fan.temp3` - Level 3 threshold (default: 75000 = 75°C)

### Hysteresis Values (milli-degrees Celsius)
- `hw.rpi5.cooling_fan.temp0_hyst` - Level 0 hysteresis (default: 5000 = 5°C)
- `hw.rpi5.cooling_fan.temp1_hyst` - Level 1 hysteresis (default: 5000 = 5°C)
- `hw.rpi5.cooling_fan.temp2_hyst` - Level 2 hysteresis (default: 5000 = 5°C)
- `hw.rpi5.cooling_fan.temp3_hyst` - Level 3 hysteresis (default: 5000 = 5°C)

### PWM Speed Levels (0-255)
- `hw.rpi5.cooling_fan.speed0` - Level 0 PWM (default: 75)
- `hw.rpi5.cooling_fan.speed1` - Level 1 PWM (default: 125)
- `hw.rpi5.cooling_fan.speed2` - Level 2 PWM (default: 175)
- `hw.rpi5.cooling_fan.speed3` - Level 3 PWM (default: 250)

### Current Fan State (read-only)
- `hw.rpi5.cooling_fan.current_state` - Current cooling level (0-3)

## Usage Examples

### View all cooling fan settings
```bash
sysctl -a hw.rpi5.cooling_fan
```

### View a specific setting
```bash
sysctl hw.rpi5.cooling_fan.temp0
```

### Modify a temperature threshold
```bash
# Lower the Level 0 threshold to 45°C (45000 mC)
sysctl hw.rpi5.cooling_fan.temp0=45000
```

### Modify PWM speed
```bash
# Increase Level 0 fan speed to 90 (out of 255)
sysctl hw.rpi5.cooling_fan.speed0=90
```

### Modify hysteresis
```bash
# Increase hysteresis for Level 1 to 10°C (10000 mC)
sysctl hw.rpi5.cooling_fan.temp1_hyst=10000
```

### Check current fan state
```bash
sysctl hw.rpi5.cooling_fan.current_state
```

### Make changes persistent

Add to `/etc/sysctl.conf`:
```
hw.rpi5.cooling_fan.temp0=45000
hw.rpi5.cooling_fan.speed0=90
hw.rpi5.cooling_fan.temp1_hyst=10000
```

## Thermal Behavior

The module implements a 4-level cooling system:

| Level | Temp Threshold | Default PWM | Default Hysteresis |
|-------|----------------|-------------|-------------------|
| 0     | 50°C           | 75 (29%)    | 5°C                |
| 1     | 60°C           | 125 (49%)   | 5°C                |
| 2     | 67.5°C         | 175 (69%)   | 5°C                |
| 3     | 75°C           | 250 (98%)   | 5°C                |

## Input Validation

- **Temperature values**: 0-120000 mC (0-120°C)
- **Hysteresis values**: 0-10000 mC (0-10°C)
- **PWM speed values**: 0-255
- **Current state**: Read-only (attempting to write returns EPERM)

## Thread Safety

All access to cooling fan state is protected by a mutex lock (mtx) to ensure thread-safe concurrent access from multiple processes.

## Integration with Actual Hardware

Currently, this module stores values in memory only. To integrate with actual hardware, you would need to:

1. Add hardware access functions to read/write:
   - PWM controller registers
   - Temperature sensor values
   - Fan tachometer readings

2. Modify the sysctl handlers to:
   - Write PWM values to the RP1 PWM controller
   - Read actual temperature from the thermal sensor
   - Update the `fan_current_state` based on actual fan feedback

3. Implement a thermal management daemon that:
   - Periodically reads CPU temperature
   - Updates the fan state based on configured thresholds
   - Enforces hysteresis logic

Example device tree integration points (for future enhancement):
- RP1 PWM controller: `/dev/pwm*` or via device tree
- Thermal sensor: `/sys/class/thermal/thermal_zone*`
- Device tree node: `/proc/device-tree/cooling_fan`

## Unloading

```bash
# Unload the module
kldunload rpi5_cooling_fan
```

## License

BSD 2-Clause License (SPDX-License-Identifier: BSD-2-Clause)

## See Also

- FreeBSD Kernel Module Development
- sysctl(3) - Get/set system parameters
- sysctl(8) - Sysctl command-line utility
- Raspberry Pi 5 Device Tree Reference
