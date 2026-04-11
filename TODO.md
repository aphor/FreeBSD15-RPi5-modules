# BCM2712 Thermal Sensor Implementation Plan

## Overview

This document outlines the implementation plan for replacing the stub `bcm2712_read_cpu_temp()` function with real BCM2712 thermal sensor reading. The implementation follows Raspbian OS thermal access patterns while adhering strictly to FreeBSD kernel development conventions.

## Temperature Reading Architecture

### Raspbian OS Approach
- **Hardware**: BCM2712 SoC integrated thermal sensor (part of AVS system)
- **Interface**: Reads from `/sys/class/thermal/thermal_zone0/temp` or `/sys/class/hwmon/hwmon0/temp1_input`
- **Format**: Temperature in millidegrees Celsius (milli-°C)
- **Driver**: BCM2711 AVS RO (Read-Only) thermal sensor driver
- **Units**: Divide by 1000 to get Celsius (e.g., 34862 mC = 34.862°C)

### FreeBSD Adaptation
- **sysctl Format**: Use deciKelvin (dK) format code `"IK"` per FreeBSD convention
- **Conversion**: milli-°C → dK: `(mC / 100) + 2731` (constant: TZ_ZEROC = 2731)
- **Caching Pattern**: Periodic updates via callout timer (1-second interval)
- **Thread Safety**: Mutex-protected reads and cached values
- **Model**: Follow Armada thermal driver pattern (sys/arm/mv/armada/thermal.c)

---

## Implementation Steps

### Phase 1: Hardware Detection & Initialization

#### Task 1.1: Add Thermal Sensor Resource Discovery
**Location**: `bcm2712.c` - `bcm2712_attach()` function

- [ ] Add device tree thermal zone node detection
  - Check for `thermal-sensors` property in device tree
  - Locate thermal zone reference pointing to BCM2712
  - Parse sensor index and reference offset

- [ ] Allocate thermal sensor resources in softc
  - Add to `struct bcm2712_softc`:
    ```c
    struct mtx thermal_mtx;           /* Protect thermal reads */
    struct callout thermal_callout;   /* Periodic update timer */
    uint32_t cached_temp_mc;          /* Cached milli-°C value */
    time_t last_update;               /* Timestamp of last read */
    ```
  - Initialize mutex: `mtx_init(&sc->thermal_mtx, "bcm2712_thermal", NULL, MTX_DEF)`

#### Task 1.2: Register sysctl Temperature Node
**Location**: `bcm2712.c` - `bcm2712_attach()` function

- [ ] Create sysctl tree node for thermal readings
  - Use pattern: `hw.bcm2712.thermal.cpu_temp`
  - Add sysctl handler function: `bcm2712_thermal_sysctl_temp()`
  - Register with format `"IK"` (deciKelvin)
  - Mark as read-only (CTLFLAG_RD)

- [ ] Add sysctl handler function
  ```c
  static int bcm2712_thermal_sysctl_temp(SYSCTL_HANDLER_ARGS)
  {
      struct bcm2712_softc *sc = arg1;
      uint32_t temp_dk;

      mtx_lock(&sc->thermal_mtx);
      /* Convert cached milli-°C to deciKelvin */
      temp_dk = (sc->cached_temp_mc / 100) + TZ_ZEROC;
      mtx_unlock(&sc->thermal_mtx);

      return sysctl_handle_int(oidp, &temp_dk, 0, req);
  }
  ```

---

### Phase 2: Thermal Sensor Hardware Access

#### Task 2.1: Implement Hardware Register Reading
**Location**: `bcm2712.c` - New function: `bcm2712_thermal_read_raw()`

- [ ] Create raw hardware read function
  - Parameter: `struct bcm2712_softc *sc`
  - Return: temperature in millidegrees Celsius (uint32_t)
  - Must be called with `thermal_mtx` locked

- [ ] Determine BCM2712 AVS thermal sensor register offset
  - Research BCM2712 datasheet for AVS ring oscillator block address
  - Define macro: `#define BCM2712_AVS_TEMP_OFFSET 0x????`
  - Document register layout in header comment

- [ ] Implement register read logic
  - Use existing memory-mapped I/O: `bus_space_read_4()` (already available via bst/bsh)
  - Handle endianness correctly for ARM64
  - Apply any calibration or scaling factors from datasheet

- [ ] Add error handling
  - Return sensible default on read failure (e.g., 50000 mC = 50°C)
  - Add device_printf warnings for persistent failures
  - Consider hysteresis to avoid spurious changes

#### Task 2.2: Implement Conversion Function
**Location**: `bcm2712.c` - New function: `bcm2712_thermal_raw_to_millic()`

- [ ] Create conversion function
  - Parameter: raw sensor value from hardware
  - Return: temperature in millidegrees Celsius
  - Apply datasheet-specified conversion formula if needed

- [ ] Document conversion formula
  - Include datasheet reference and calibration points
  - Add inline comments explaining scaling
  - Test against known good values from Raspbian comparison

---

### Phase 3: Periodic Temperature Updates

#### Task 3.1: Implement Callout Timer Handler
**Location**: `bcm2712.c` - New function: `bcm2712_thermal_update()`

- [ ] Create callout callback function
  ```c
  static void bcm2712_thermal_update(void *arg)
  {
      struct bcm2712_softc *sc = arg;
      uint32_t temp_raw;

      mtx_assert(&sc->thermal_mtx, MA_OWNED);

      /* Read hardware and cache result */
      temp_raw = bcm2712_thermal_read_raw(sc);
      sc->cached_temp_mc = bcm2712_thermal_raw_to_millic(temp_raw);
      sc->last_update = time_uptime;

      /* Reschedule for next update */
      callout_reset(&sc->thermal_callout, hz, bcm2712_thermal_update, sc);
  }
  ```

- [ ] Initialize callout in attach()
  - Call after softc allocation: `callout_init_mtx(&sc->thermal_callout, &sc->thermal_mtx, 0)`
  - Schedule first update: `callout_reset(&sc->thermal_callout, hz, bcm2712_thermal_update, sc)`

#### Task 3.2: Ensure Callout Cleanup in Detach
**Location**: `bcm2712.c` - `bcm2712_detach()` function

- [ ] Add callout drain before device removal
  - **CRITICAL**: Must drain callout before freeing softc to prevent use-after-free
  - Call: `callout_drain(&sc->thermal_callout)` with mutex held
  - Verify no pending callbacks remain

---

### Phase 4: Public API for Other Modules

#### Task 4.1: Update bcm2712_read_cpu_temp() Wrapper
**Location**: `bcm2712.c` - Modify existing function: `bcm2712_read_cpu_temp()`

- [ ] Replace stub with proper implementation
  - Acquire thermal_mtx lock
  - Return cached value: `*temp = sc->cached_temp_mc`
  - Release lock before returning

- [ ] Add parameter validation
  - Check if `temp` pointer is valid (not NULL)
  - Return EINVAL on invalid parameters
  - Return ENODEV if thermal sensor not initialized

#### Task 4.2: Add Direct Millidegree API
**Location**: `bcm2712_var.h` - Update header exports

- [ ] Add high-level API function to header
  ```c
  /* Get current CPU temperature in millidegrees Celsius */
  int bcm2712_read_cpu_temp_millic(uint32_t *temp_mc);
  ```

- [ ] Update documentation comments
  - Document return format (millidegrees Celsius)
  - Document return values (0 on success, errno on failure)
  - Note about caching (1-second update interval)

---

### Phase 5: Integration with rpi5 Module

#### Task 5.1: Update rpi5.c to Use Real Temperature
**Location**: `rpi5.c` - Modify thermal management loop

- [ ] Replace placeholder temperature calls
  - Change from hardcoded 50°C to real value
  - Use `bcm2712_read_cpu_temp()` for milli-°C reading
  - No conversion needed (already in milli-°C format)

- [ ] Update sysctl temperature reading
  - Verify `hw.rpi5.cooling_fan.cpu_temp` gets real value
  - Confirm format is millidegrees Celsius throughout

---

### Phase 6: Testing & Validation

#### Task 6.1: Unit Tests
**Location**: `test/test_thermal.sh` (new file)

- [ ] Create thermal sensor test suite
  - [ ] Test sysctl node presence: `sysctl hw.bcm2712.thermal.cpu_temp`
  - [ ] Test temperature range validation (must be > 0°C, < 150°C)
  - [ ] Test sysctl format is numeric (deciKelvin)
  - [ ] Test stability over 10 seconds (no huge jumps)
  - [ ] Compare with Raspbian `/sys/class/thermal/thermal_zone0/temp` if available

#### Task 6.2: Integration Tests
**Location**: `test/` directory - Update existing test suite

- [ ] Verify rpi5 module uses real temperature
  - Load both modules: `sudo kldload bcm2712 rpi5`
  - Verify cpu_temp shows realistic value: `sysctl hw.rpi5.cooling_fan.cpu_temp`
  - Confirm temperature changes with system load (heat generation)

#### Task 6.3: Stress Tests
**Location**: `test/` directory - Add thermal stress test

- [ ] Run thermal stability test under load
  - Start CPU-intensive workload (compile or dd)
  - Monitor temperature readings for 5 minutes
  - Verify smooth updates without crashes or freezes
  - Check dmesg for any thermal-related errors

---

### Phase 7: Documentation

#### Task 7.1: Update Code Comments
**Location**: `bcm2712.c` and `bcm2712_var.h`

- [ ] Add BCM2712 thermal sensor documentation
  - Document AVS sensor block location and register layout
  - Explain temperature reading mechanism and units
  - Reference BCM2712 datasheet sections

- [ ] Document conversion formula
  - Include formula derivation
  - List calibration points if applicable
  - Cross-reference with Raspbian implementation

#### Task 7.2: Update CLAUDE.md
**Location**: `CLAUDE.md`

- [ ] Add thermal sensor architecture section
  - Explain hardware interface (AVS sensor)
  - Document sysctl temperature interface
  - Describe periodic update mechanism

- [ ] Add troubleshooting section
  - Invalid temperature values
  - Thermal callout issues
  - How to verify sensor functionality

---

## Implementation Dependencies

### Required
- Device tree thermal zone definition for BCM2712
- BCM2712 datasheet (thermal sensor register map)
- Understanding of Armada thermal driver pattern (reference implementation)

### Optional (for validation)
- Raspbian comparison tool to verify readings
- Stress test tools (stress-ng, burnMMC)
- Thermal camera for validation of accuracy

---

## Code References & Models

### FreeBSD Driver Examples
- **Armada Thermal (RECOMMENDED)**: `/usr/src/sys/arm/mv/armada/thermal.c`
  - Line 300-309: Periodic update pattern
  - Line 233-245: Callout initialization
  - Matches requirements: periodic reads, sysctl export, mutex protection

- **Allwinner Thermal**: `/usr/src/sys/arm/allwinner/aw_thermal.c`
  - Line 493: sysctl handler function
  - Alternative pattern: on-demand reads (less suitable for constant fan control)

- **IMX ANATOP**: `/usr/src/sys/arm/freescale/imx/imx6_anatop.c`
  - Line 629: Temperature conversion and sysctl exposure
  - Pattern: hardware read → conversion → handler return

### FreeBSD Kernel APIs
- **Thread Synchronization**: `mtx_*` functions (sys/sys/mutex.h)
- **Timers**: `callout_*` functions (sys/sys/callout.h)
- **sysctl**: `SYSCTL_ADD_PROC()` macro (sys/sys/sysctl.h)
- **I/O Access**: `bus_space_read_4()` (sys/sys/bus.h)

---

## FreeBSD Convention Checklist

- [ ] Use deciKelvin (dK) format for sysctl temperature exports
- [ ] Mutex-protect all hardware access
- [ ] Use callouts for periodic updates (not kthreads)
- [ ] Drain callouts in detach to prevent use-after-free
- [ ] Follow existing BCM2712 code style (tabs, naming conventions)
- [ ] Add proper error checking and device_printf warnings
- [ ] Document with FreeBSD-style comments (/* */ for blocks)
- [ ] Include proper SPDX license headers
- [ ] Test with FreeBSD kernel module loading/unloading
- [ ] Verify no memory leaks with `kgdb` or valgrind equivalent

---

## Success Criteria

1. **Temperature Reading**: Real sensor values appear in sysctl output
2. **Format Compliance**: Temperature in deciKelvin format via `hw.bcm2712.thermal.cpu_temp`
3. **RPi5 Integration**: `hw.rpi5.cooling_fan.cpu_temp` shows real value, fan responds to heat
4. **Stability**: No kernel panics or deadlocks during normal operation
5. **Cleanup**: No memory leaks or use-after-free on module unload
6. **Documentation**: Clear comments explaining thermal sensor hardware and conversion
7. **Testing**: All unit, integration, and stress tests pass

---

## Timeline Estimate

- Phase 1 (Detection/Init): 1-2 hours
- Phase 2 (Hardware Access): 2-3 hours
- Phase 3 (Periodic Updates): 1 hour
- Phase 4-5 (API/Integration): 1 hour
- Phase 6 (Testing): 2-3 hours
- Phase 7 (Documentation): 1 hour

**Total**: ~8-12 hours of active development + research time

---

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| Incorrect register offset | Verify with BCM2712 datasheet, compare Raspbian source |
| Temperature calibration wrong | Cross-check readings with Raspbian, adjust formula |
| Callout deadlock | Follow Armada pattern, use `mtx_assert()` for verification |
| Memory leak on detach | Use `callout_drain()`, free all allocated resources |
| Performance impact | Periodic reads are minimal (1/sec), monitor CPU usage |

---

## Notes for Future Implementation

- Consider device tree thermal zone cooling bindings if adding active cooling policies
- May want to add hysteresis thresholds to prevent oscillation
- Could add per-core temperature reading if BCM2712 provides it
- Consider integration with FreeBSD's thermal zone framework (thermzone(4)) for advanced policies
