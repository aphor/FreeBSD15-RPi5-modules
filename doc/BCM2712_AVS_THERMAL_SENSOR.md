# BCM2712 AVS Thermal Sensor Hardware Research

**Research Date:** 2026-03-28
**Target:** BCM2712 (Raspberry Pi 5) and BCM2711 (Raspberry Pi 4) AVS thermal sensor implementation

## 1. Physical/Virtual Memory Address

### AVS Monitor Base Address
- **Memory Address:** `0x7d5d2000` (BCM2711/BCM2712)
- **Register Range Size:** `0xf00` (3840 bytes)
- **Device Compatibility String:** `brcm,bcm2711-avs-monitor` (used as syscon/mfd)

### Register Offset for Temperature Reading
- **AVS_RO_TEMP_STATUS Register Offset:** `0x200` (relative to base 0x7d5d2000)
- **Full Address:** `0x7d5d2000 + 0x200 = 0x7d5d2200`

### Alternative Configuration (Fixed Address)
Some kernel versions use an alternate fixed mapping:
- **Direct Address:** `0x7d5d2200`
- **Range Size:** `0x4` (single register)
- **Offset from this base:** `0x0`

## 2. Raw Value Format and Register Layout

### AVS_RO_TEMP_STATUS Register Structure

```
Bit Layout:
Bits 0-9:   Temperature data (10 bits) - GENMASK(9, 0)
Bits 10-15: Reserved
Bit 16:     Validity flag (part of AVS_RO_TEMP_STATUS_VALID_MSK)
Bits 17-31: Reserved

Validation Mask: BIT(16) | BIT(10) = 0x0410
Data Mask: GENMASK(9, 0) = 0x03FF (bits 0-9)
```

### Data Extraction in Kernel Code

```c
/* From Linux thermal/broadcom/bcm2711_thermal.c */
#define AVS_RO_TEMP_STATUS              0x200
#define AVS_RO_TEMP_STATUS_DATA_MSK     GENMASK(9, 0)   /* Bits 0-9 */
#define AVS_RO_TEMP_STATUS_VALID_MSK    (BIT(16) | BIT(10))

/* Reading process */
uint32_t raw_value;
regmap_read(regmap, AVS_RO_TEMP_STATUS, &raw_value);
if (!(raw_value & AVS_RO_TEMP_STATUS_VALID_MSK))
    return -EINVAL;  /* Invalid reading */
uint32_t temp_code = raw_value & AVS_RO_TEMP_STATUS_DATA_MSK;
```

## 3. Scaling/Conversion Formula to Celsius

### Linear Calibration Formula

The BCM2711/BCM2712 AVS sensor uses a linear two-point calibration formula:

```
Temperature (millidegrees Celsius) = slope × raw_code + offset
```

### Calibration Parameters

#### Typical Values (Raspberry Pi 4/5 Default)
- **Slope:** `-550` (negative, indicating inverse relationship)
- **Offset:** `450000` (millidegrees Celsius)
- **Output Unit:** millidegrees Celsius (divide by 1000 for °C)

#### How Parameters Work
1. Raw sensor code (0-1023) is read from bits 0-9
2. Multiply by slope: `raw_code × (-550)`
3. Add offset: `result + 450000`
4. Result is temperature in millidegrees Celsius (mC)

### Example Conversion
```
raw_code = 512 (mid-range)
temp_mC = 512 × (-550) + 450000
temp_mC = -281600 + 450000
temp_mC = 168400 mC = 168.4°C (likely boot conditions)

raw_code = 700
temp_mC = 700 × (-550) + 450000
temp_mC = -385000 + 450000
temp_mC = 65000 mC = 65°C (typical operating)
```

### FreeBSD Conversion (deciKelvin)

For FreeBSD kernel modules (as used in rpi5_modules.git):

```c
#define TZ_ZEROC 2731  /* Constant in FreeBSD */

/* Convert millidegrees Celsius to deciKelvin */
temp_dK = (temp_mC / 100) + TZ_ZEROC

/* Example: 65000 mC */
temp_dK = (65000 / 100) + 2731 = 650 + 2731 = 3381 dK
/* Which equals 65°C in Celsius */
```

## 4. Calibration Factors and Offsets

### Device Tree Configuration

The calibration parameters are stored in the device tree thermal zone definition (not hardcoded):

```dts
/* From bcm2711.dtsi or bcm2712.dtsi */
thermal-zones {
    soc_thermal: soc-thermal {
        polling-delay-passive = <1000>;  /* ms */
        polling-delay = <1000>;           /* ms */

        thermal-sensors = <&avs_monitor 0>;

        trips {
            warning: trip-point-0 {
                temperature = <80000>;    /* mC (80°C) */
                hysteresis = <2000>;      /* mC */
                type = "passive";
            };
            critical: trip-point-1 {
                temperature = <95000>;    /* mC (95°C) */
                hysteresis = <0>;
                type = "critical";
            };
        };
    };
};

avs_monitor: avs-monitor@7d5d2000 {
    compatible = "brcm,bcm2711-avs-monitor", "syscon", "simple-mfd";
    reg = <0x0 0x7d5d2000 0x0 0xf00>;
    /* Calibration values used by thermal driver */
    #thermal-sensor-cells = <0>;
};

thermal_sensor: thermal-sensor {
    compatible = "brcm,bcm2711-thermal";
    /* Slope and offset are queried from thermal zone at runtime */
};
```

### Runtime Calibration Retrieval

In the bcm2711_thermal.c driver:

```c
int bcm2711_get_temp(struct thermal_zone_device *tz, int *temp)
{
    struct bcm2711_thermal_priv *priv = tz->devdata;
    uint32_t raw_code;
    int slope, offset;

    /* Read raw sensor value */
    regmap_read(priv->regmap, AVS_RO_TEMP_STATUS, &raw_code);

    /* Validate reading */
    if (!(raw_code & AVS_RO_TEMP_STATUS_VALID_MSK))
        return -EINVAL;

    /* Extract temperature bits */
    raw_code &= AVS_RO_TEMP_STATUS_DATA_MSK;

    /* Get calibration parameters from thermal zone */
    slope = thermal_zone_get_slope(tz);      /* Typically -550 */
    offset = thermal_zone_get_offset(tz);    /* Typically 450000 */

    /* Apply linear conversion */
    *temp = slope * raw_code + offset;       /* Result in mC */

    return 0;
}
```

### Calibration Factor Details

| Parameter | Value | Unit | Notes |
|-----------|-------|------|-------|
| Slope | -550 | mC/code | Negative = inverse relationship |
| Offset | 450000 | mC | Base temperature offset |
| Max raw code | 1023 | LSBs | 10-bit sensor |
| Min temperature | ~-11150 | mC | At raw_code=1023 |
| Max temperature | 450000 | mC | At raw_code=0 |
| Typical operating | 30000-75000 | mC | 30-75°C |

## 5. Validity Checking

The sensor includes built-in validity indicators:

```c
/* Valid reading requires both bits 16 and 10 to be set */
#define AVS_RO_TEMP_STATUS_VALID_MSK (BIT(16) | BIT(10))

bool is_valid = (raw_value & AVS_RO_TEMP_STATUS_VALID_MSK) ==
                AVS_RO_TEMP_STATUS_VALID_MSK;
```

If validity mask check fails, the reading is stale/invalid and should be discarded.

## 6. Hardware Implementation Notes

### Sensor Type: AVS Ring Oscillator

The BCM2711/BCM2712 uses an **Adaptive Voltage Scaling (AVS) ring oscillator** for temperature sensing instead of a dedicated thermal sensor block.

**Why this matters:**
- Ring oscillators have frequency that varies with temperature
- The frequency is counted and converted to a digital code
- This approach is less accurate than dedicated sensors but lower cost
- Requires careful calibration (slope/offset parameters)

### Access Pattern: regmap/syscon

The thermal driver accesses registers through Linux's **regmap** abstraction:
- The AVS monitor is registered as a `syscon` (system controller)
- The thermal driver obtains a regmap handle to the syscon
- All register reads go through `regmap_read(regmap, offset, &value)`
- This provides proper synchronization and error handling

```c
/* In bcm2711_thermal probe function */
struct regmap *regmap;
regmap = syscon_node_to_regmap(np->parent);
if (IS_ERR(regmap))
    return PTR_ERR(regmap);
priv->regmap = regmap;
```

### Update Frequency

The thermal sensor is polled periodically by the thermal subsystem:
- **Passive polling:** 1000 ms (1 second) - default interval
- **Active polling:** During critical conditions may be faster
- Not a continuous streaming interface

## 7. FreeBSD Porting Considerations

### Challenges for Kernel Module Implementation

1. **No regmap abstraction in FreeBSD:** Must directly map physical address via bus_space
2. **No device tree thermal zones:** Must define calibration in code
3. **Memory mapping required:** Physical address 0x7d5d2000 needs VM mapping
4. **Synchronization:** Mutex protection for register access

### Recommended FreeBSD Implementation

```c
/* In bcm2712.c */
#define BCM2712_AVS_BASE        0x7d5d2000
#define BCM2712_AVS_TEMP_OFFSET 0x200

/* Calibration constants */
#define BCM2712_TEMP_SLOPE      -550      /* mC per code point */
#define BCM2712_TEMP_OFFSET     450000    /* mC at code 0 */
#define BCM2712_TEMP_VALID_MSK  0x0410    /* Bits 16 and 10 */
#define BCM2712_TEMP_DATA_MSK   0x03FF    /* Bits 0-9 */

/* Temperature reading function */
static uint32_t
bcm2712_thermal_read_raw(struct bcm2712_softc *sc)
{
    uint32_t raw_value;

    mtx_assert(&sc->thermal_mtx, MA_OWNED);

    /* Read AVS_RO_TEMP_STATUS register */
    raw_value = bus_space_read_4(sc->bst, sc->bsh,
                                 BCM2712_AVS_TEMP_OFFSET);

    /* Check validity */
    if ((raw_value & BCM2712_TEMP_VALID_MSK) != BCM2712_TEMP_VALID_MSK) {
        /* Invalid reading, return cached value or error */
        return (0);
    }

    return (raw_value);
}

/* Conversion function */
static uint32_t
bcm2712_thermal_raw_to_millic(uint32_t raw_value)
{
    uint32_t raw_code;
    int32_t temp_mc;

    /* Extract 10-bit temperature code */
    raw_code = raw_value & BCM2712_TEMP_DATA_MSK;

    /* Apply linear calibration formula */
    temp_mc = (BCM2712_TEMP_SLOPE * raw_code) + BCM2712_TEMP_OFFSET;

    /* Clamp to reasonable range */
    if (temp_mc < 0)
        temp_mc = 0;
    if (temp_mc > 120000)  /* Cap at 120°C */
        temp_mc = 120000;

    return ((uint32_t)temp_mc);
}
```

## 8. References and Sources

### Official Raspberry Pi Documentation
- [Processors - Raspberry Pi Documentation](https://www.raspberrypi.com/documentation/computers/processors.html)
- [BCM2712 SoC Documentation](https://github.com/raspberrypi/documentation/)

### Linux Kernel Source Code
- [bcm2711_thermal.c (Raspberry Pi Linux rpi-6.6.y)](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/thermal/broadcom/bcm2711_thermal.c)
- [bcm2711_thermal.c (Mainline Linux)](https://github.com/torvalds/linux/blob/master/drivers/thermal/broadcom/bcm2711_thermal.c)
- [bcm2711.dtsi (Raspberry Pi Device Tree)](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm2711.dtsi)
- [bcm2712.dtsi (Raspberry Pi 5 Device Tree)](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm64/boot/dts/broadcom/bcm2712.dtsi)

### Kernel Patches and Design Documentation
- [BCM2711 Thermal Driver Patch (v4)](https://patchwork.kernel.org/project/linux-arm-kernel/patch/1578759342-4550-3-git-send-email-stefan.wahren@i2se.com/)
- [dt-bindings: Broadcom AVS RO Thermal Binding](https://patchwork.kernel.org/project/linux-pm/patch/1578420957-32229-2-git-send-email-wahrenst@gmx.net/)

### Community Resources
- [Accessing RP1 Peripherals from BCM2712 (Raspberry Pi Forums)](https://forums.raspberrypi.com/viewtopic.php?t=368402)
- [BCM2712 SoC Documentation (DeepWiki)](https://deepwiki.com/raspberrypi/linux/2.3-bcm2712-soc-(raspberry-pi-5))
- [Thermal Management Guide](https://emlogic.no/2024/09/step-by-step-thermal-management/)

### Datasheets
- [BCM2711 Peripherals (Requires Redirect)](https://pip.raspberrypi.com/documents/RP-008248-DS-bcm2711-peripherals.pdf)
- [RP1 Peripherals (Requires Redirect)](https://pip.raspberrypi.com/documents/RP-008370-DS-rp1-peripherals.pdf)

## 9. Key Takeaways for Implementation

1. **Temperature Register Location:** `0x7d5d2200` (physical address)
2. **Data Extraction:** 10-bit code in bits 0-9 of register offset 0x200
3. **Validity Check:** Both bits 16 and 10 must be set
4. **Conversion Formula:** `temp_mC = slope(-550) × raw_code + offset(450000)`
5. **Output Format:** Millidegrees Celsius (divide by 1000 for °C)
6. **Calibration:** Device-specific slope/offset in device tree (fixed values for Pi 4/5)
7. **Hardware Type:** AVS ring oscillator (not dedicated thermal block)
8. **Access Method:** Requires physical memory mapping for FreeBSD modules

## 10. Known Issues and Workarounds

### Kernel Panic with 0xf00 Range (Fixed)
Original attempt to map full 0xf00 range caused SError interrupts. Solution: Use direct address 0x7d5d2200 with 0x4 byte range, or use proper device tree syscon mapping.

### Temperature Noise
Ring oscillator sensors can be noisy. Recommended: Apply low-pass filtering or median filtering to multiple readings.

### No Absolute Calibration
Slope/offset are relative calibration only. Absolute accuracy depends on factory calibration factors not exposed in device tree.
