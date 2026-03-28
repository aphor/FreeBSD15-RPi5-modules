# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains FreeBSD kernel modules for Raspberry Pi 5 cooling fan control. The project is split into two modules:
- **bcm2712**: Common BCM2712 hardware support (RP1 PWM controller, thermal sensor access)
- **rpi5**: Pi 5-specific board support (cooling fan control, thermal management)

The rpi5 module depends on bcm2712 and automatically loads it when requested.

## Build System

### Build Commands

**Build all modules (default):**
```bash
make
# or explicitly
make all
```

**Build individual modules:**
```bash
make bcm2712  # Build BCM2712 common hardware module only
make rpi5     # Build RPi5 board-specific module only
```

**Install modules:**
```bash
sudo make install              # Install all modules
sudo make install-bcm2712      # Install BCM2712 hardware module only
sudo make install-rpi5         # Install RPi5 board module only
```

**Complete build and test:**
```bash
sudo make test  # Build, install, load, and check status
```

### Build System Configuration

**System Headers:**
The Makefiles are configured to use FreeBSD system headers instead of embedded copies:
- **Interface headers** (device_if.h, bus_if.h, pwmbus_if.h) are auto-generated from system .m files
- **Configuration headers** (opt_fdt.h, opt_global.h) are created automatically during build
- **Include paths** reference system directories:
  - `-I/usr/src/sys` - Main kernel source
  - `-I/usr/src/sys/contrib/libfdt` - FDT (Flattened Device Tree) support
  - `-I/usr/src/sys/dev/fdt` - FDT device framework
  - `-I/usr/src/sys/dev/pwm` - PWM subsystem (bcm2712 only)

**Auto-generated Files:**
The build system automatically generates required interface headers:
```bash
# Generated during build from system sources:
device_if.h    # From /usr/src/sys/kern/device_if.m
bus_if.h       # From /usr/src/sys/kern/bus_if.m
pwmbus_if.h    # From /usr/src/sys/dev/pwm/pwmbus_if.m
opt_fdt.h      # Configuration for FDT support
opt_global.h   # Global kernel configuration
```

**Clean embedded headers:**
No header files are embedded in the source tree - all headers are either project-specific (`bcm2712_var.h`) or auto-generated from system sources.

### Module Loading

**Load modules using Makefile:**
```bash
sudo make load              # Load all modules
sudo make load-bcm2712      # Load BCM2712 hardware module only
sudo make load-rpi5         # Load RPi5 board module (auto-loads bcm2712)
```

**Load modules manually:**
```bash
sudo kldload bcm2712        # Load hardware support first
sudo kldload rpi5           # Load board support (or just this - auto-loads bcm2712)
```

**Unload modules:**
```bash
sudo make unload            # Unload all modules (rpi5 first, then bcm2712)
sudo make unload-rpi5       # Unload RPi5 board module only
sudo make unload-bcm2712    # Unload BCM2712 hardware module only
```

**Check module status:**
```bash
make status                 # Check load status and sysctl interface
kldstat | grep -E "(bcm2712|rpi5)"  # Manual check
```

**Auto-load at boot (add to /boot/loader.conf):**
```
bcm2712_load="YES"
rpi5_load="YES"
# Note: Only rpi5_load is needed - it auto-loads bcm2712
```

### Testing and Monitoring

**Use the control script:**
```bash
chmod +x rpi5_fan_control_integrated.sh

# Check module status
./rpi5_fan_control_integrated.sh --check

# Run full diagnostics
./rpi5_fan_control_integrated.sh --diagnostics

# Monitor fan in real-time
./rpi5_fan_control_integrated.sh --monitor

# Test PWM hardware
./rpi5_fan_control_integrated.sh --test
```

**Manual sysctl commands:**
```bash
# View all cooling fan settings
sysctl -a hw.rpi5.cooling_fan

# Set temperature threshold to 45°C
sysctl hw.rpi5.cooling_fan.temp0=45000

# Set PWM speed (0-255)
sysctl hw.rpi5.cooling_fan.speed0=100
```

## Architecture

### Component Structure

1. **BCM2712 Common Hardware Module** (`bcm2712.c`)
   - RP1 PWM controller driver with pwmbus interface
   - Manages 4 PWM channels with 125 MHz clock
   - BCM2712 thermal sensor access functions
   - Thread-safe register access via mutex
   - Provides common hardware abstraction for BCM2712-based boards

2. **RPi5 Board-Specific Module** (`rpi5.c`)
   - Raspberry Pi 5 cooling fan thermal management
   - Temperature monitoring with 1-second polling
   - 4-level fan control with hysteresis logic
   - sysctl interface under `hw.rpi5.cooling_fan.*`
   - Depends on bcm2712 module for hardware access
   - Automatic module dependency loading

3. **Control Script** (`rpi5_fan_control_integrated.sh`)
   - Shell-based management interface
   - Diagnostics, monitoring, and preset configurations
   - Module loading verification for both bcm2712 and rpi5
   - Handles dependency relationships automatically

### Hardware Integration

**PWM Configuration:**
- Uses PWM channel 3 on RP1 controller (pwmc2.3 or pwmc3.3)
- Period: 41566 ns (~24 kHz frequency)
- Inverted polarity (0% duty = fan off, 100% = fan on)
- PWM range: 0-255 (mapped to 0-100% duty cycle)

**Temperature Sources:**
- Currently uses placeholder function returning 50°C
- Designed for integration with BCM2712 thermal sensor
- Can be modified to read from hwmon or device tree thermal zones

**Thermal Control Logic:**
```
Level 0: <50°C  → Fan off (0 PWM)
Level 1: 50-60°C → Low speed (75 PWM, ~29%)
Level 2: 60-67.5°C → Medium speed (125 PWM, ~49%)
Level 3: 67.5-75°C → High speed (175 PWM, ~69%)
Level 4: >75°C → Max speed (250 PWM, ~98%)
```

### sysctl Interface

**Configurable parameters:**
- `hw.rpi5.cooling_fan.temp{0-3}` - Temperature thresholds (milli-°C)
- `hw.rpi5.cooling_fan.temp{0-3}_hyst` - Hysteresis values (milli-°C)
- `hw.rpi5.cooling_fan.speed{0-3}` - PWM speeds (0-255)

**Read-only status:**
- `hw.rpi5.cooling_fan.cpu_temp` - Current CPU temperature (milli-°C)
- `hw.rpi5.cooling_fan.current_state` - Current fan level (0-3)

## Key Files

- `bcm2712.c` - BCM2712 common hardware support module
- `bcm2712_var.h` - BCM2712 module header and API definitions
- `rpi5.c` - RPi5 board-specific thermal management module
- `rpi5_fan_control_integrated.sh` - Management script
- `Makefile.bcm2712` - Build configuration for BCM2712 module
- `Makefile.rpi5` - Build configuration for RPi5 module
- `Makefile` - Consolidated build system for both modules
- `BUILDING.md` - Detailed build and installation instructions
- `INTEGRATION_GUIDE.md` - Complete architecture and integration documentation

## Common Development Tasks

**Modify temperature thresholds:**
Edit default values in `rpi5.c` around lines 40-50, then rebuild.

**Add new PWM channels:**
Extend `bcm2712.c` to support additional PWM outputs beyond channel 3.

**Integrate real temperature sensor:**
Replace placeholder function `bcm2712_read_cpu_temp()` in bcm2712 module with actual BCM2712 thermal sensor access.

**Add support for other BCM2712 boards:**
Create new board-specific modules (similar to rpi5.c) that depend on bcm2712 for hardware access.

**Test changes:**
1. Build modules: `make`
2. Unload existing: `sudo make unload`
3. Install and load: `sudo make install load`
4. Verify: `make status` or `./rpi5_fan_control_integrated.sh --diagnostics`

**Module dependency testing:**
1. Load only rpi5: `sudo kldload rpi5` (should auto-load bcm2712)
2. Check both loaded: `kldstat | grep -E "(bcm2712|rpi5)"`
3. Unload rpi5: `sudo kldunload rpi5` (leaves bcm2712 loaded)
4. Unload bcm2712: `sudo kldunload bcm2712`

## Device Tree Requirements

The PWM driver requires a device tree node for the RP1 PWM controller:

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

This should be added to the FreeBSD device tree overlay for Raspberry Pi 5.