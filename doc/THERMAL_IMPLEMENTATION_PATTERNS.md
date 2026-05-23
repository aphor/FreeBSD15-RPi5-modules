# FreeBSD Thermal Sensor Implementation Patterns for RPi5 BCM2712

This document provides practical code templates for implementing thermal sensor reading in the BCM2712 and RPi5 modules.

---

## 1. Pattern A: Cached Temperature with Periodic Update (Recommended)

This pattern is **efficient and clean**, used by Armada and recommended for RPi5 modules.

### 1.1 Header File Addition (bcm2712_var.h)

```c
/* Temperature sensor management */
struct bcm2712_thermal {
    int                temp_deci_kelvin;  /* Current temp in deci-Kelvin */
    struct mtx         temp_mtx;          /* Mutex for temperature access */
    struct callout     temp_update;       /* Periodic update callout */
};
```

### 1.2 Driver Softc Structure Update

```c
struct bcm2712_softc {
    /* ... existing fields ... */

    /* Thermal sensor support */
    struct bcm2712_thermal thermal;
};
```

### 1.3 Temperature Reading Function

```c
/*
 * Read CPU/SoC temperature from hardware
 * Returns temperature in milli-Celsius, or negative error code
 */
static int
bcm2712_read_cpu_temp_millic(struct bcm2712_softc *sc)
{
    uint32_t reg;
    int raw_temp;
    int millic;

    /* Example: Read from hypothetical BCM2712 thermal register */
    reg = RD4(sc, BCM2712_THERMAL_TEMP_REG);

    /* Extract temperature value (varies by hardware) */
    raw_temp = (reg >> BCM2712_THERMAL_TEMP_SHIFT) & BCM2712_THERMAL_TEMP_MASK;

    /* Validate reading */
    if (!(reg & BCM2712_THERMAL_TEMP_VALID)) {
        return (-1);  /* Invalid reading */
    }

    /* Convert raw value to milli-Celsius
     * Example formula: millic = (raw_val - BASE) * SCALE / DIV
     */
    millic = ((raw_temp - BCM2712_TEMP_BASE) * BCM2712_TEMP_SCALE) /
             BCM2712_TEMP_DIV;

    return (millic);
}
```

### 1.4 Temperature Conversion Function

```c
/*
 * Convert milli-Celsius to deci-Kelvin for sysctl
 * sysctl format "IK" expects deci-Kelvin
 * 0°C = 273.15 K ≈ 2731 in deci-Kelvin
 */
static int
bcm2712_millic_to_deci_kelvin(int millic)
{
    /* Convert mC to dC first: divide by 100 */
    int deci_celsius = millic / 100;

    /* Convert dC to dK: add 2731 */
    return (deci_celsius + 2731);
}
```

### 1.5 Periodic Update Callout

```c
/*
 * Background thread that updates temperature periodically.
 * Called every hz ticks (typically 1 second).
 */
static void
bcm2712_temp_update(void *arg)
{
    struct bcm2712_softc *sc = arg;
    int millic;

    mtx_lock(&sc->thermal.temp_mtx);

    /* Read current temperature */
    millic = bcm2712_read_cpu_temp_millic(sc);

    /* Update cached value if valid */
    if (millic >= 0) {
        sc->thermal.temp_deci_kelvin = bcm2712_millic_to_deci_kelvin(millic);
    }

    mtx_unlock(&sc->thermal.temp_mtx);

    /* Reschedule for next update */
    callout_reset(&sc->thermal.temp_update, hz, bcm2712_temp_update, sc);
}
```

### 1.6 Sysctl Handler

```c
/*
 * Sysctl handler for temperature
 * Provides read access to cached temperature value
 */
static int
bcm2712_temp_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct bcm2712_softc *sc = arg1;
    int temp;

    mtx_lock(&sc->thermal.temp_mtx);
    temp = sc->thermal.temp_deci_kelvin;
    mtx_unlock(&sc->thermal.temp_mtx);

    return (sysctl_handle_int(oidp, &temp, 0, req));
}
```

### 1.7 Initialization (in attach)

```c
static int
bcm2712_attach(device_t dev)
{
    struct bcm2712_softc *sc = device_get_softc(dev);
    struct sysctl_ctx_list *sctx;
    struct sysctl_oid_list *schildren;

    /* ... existing initialization ... */

    /* Initialize thermal subsystem */
    mtx_init(&sc->thermal.temp_mtx, "bcm2712 thermal", NULL, MTX_DEF);
    callout_init_mtx(&sc->thermal.temp_update, &sc->thermal.temp_mtx, 0);

    /* Perform initial temperature read */
    bcm2712_temp_update(sc);

    /* Schedule periodic updates (every 1 second) */
    callout_reset(&sc->thermal.temp_update, hz, bcm2712_temp_update, sc);

    /* Register sysctl interface */
    sctx = device_get_sysctl_ctx(dev);
    schildren = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));

    SYSCTL_ADD_PROC(sctx, schildren, OID_AUTO, "temperature",
        CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
        sc, 0, bcm2712_temp_sysctl, "IK",
        "BCM2712 CPU/SoC temperature in deciKelvin");

    return (0);
}
```

### 1.8 Cleanup (in detach)

```c
static int
bcm2712_detach(device_t dev)
{
    struct bcm2712_softc *sc = device_get_softc(dev);

    /* ... existing cleanup ... */

    /* Stop temperature updates */
    callout_drain(&sc->thermal.temp_update);
    mtx_destroy(&sc->thermal.temp_mtx);

    return (0);
}
```

---

## 2. Pattern B: On-Demand Reading (Alternative)

If polling is not suitable, read temperature on every sysctl access.

### 2.1 Handler with On-Demand Read

```c
/*
 * Sysctl handler that reads temperature on each access
 * More CPU expensive but always provides current value
 */
static int
bcm2712_temp_live_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct bcm2712_softc *sc = arg1;
    int millic, deci_kelvin;

    /* Read current hardware value */
    millic = bcm2712_read_cpu_temp_millic(sc);

    if (millic < 0) {
        /* Sensor error - return last known value or error */
        return (ENXIO);
    }

    /* Convert to deci-Kelvin */
    deci_kelvin = bcm2712_millic_to_deci_kelvin(millic);

    return (sysctl_handle_int(oidp, &deci_kelvin, 0, req));
}
```

### 2.2 Registration

```c
SYSCTL_ADD_PROC(sctx, schildren, OID_AUTO, "temperature",
    CTLTYPE_INT | CTLFLAG_RD,
    sc, 0, bcm2712_temp_live_sysctl, "IK",
    "BCM2712 CPU/SoC temperature (live reading)");
```

---

## 3. Pattern C: RPi5 Module Using BCM2712 Sensor

For the RPi5 module that depends on BCM2712:

### 3.1 Get Temperature from BCM2712

```c
/*
 * Query temperature from BCM2712 module
 * RPi5 module calls this to get current temperature
 */
int
bcm2712_get_temperature(void)
{
    device_t bcm2712_dev;
    struct bcm2712_softc *sc;

    /* Find BCM2712 device */
    bcm2712_dev = devclass_get_device(devclass_find("bcm2712"), 0);
    if (bcm2712_dev == NULL)
        return (-1);

    sc = device_get_softc(bcm2712_dev);
    if (sc == NULL)
        return (-1);

    mtx_lock(&sc->thermal.temp_mtx);
    int temp = sc->thermal.temp_deci_kelvin;
    mtx_unlock(&sc->thermal.temp_mtx);

    return (temp);
}
```

### 3.2 Use in RPi5 Module

```c
/* In rpi5.c fan control logic */
static void
rpi5_update_fan_speed(struct rpi5_softc *sc)
{
    int temp_dk;
    int new_pwm;

    /* Get temperature from BCM2712 */
    temp_dk = bcm2712_get_temperature();
    if (temp_dk < 0) {
        /* Error reading temperature */
        return;
    }

    /* Convert deci-Kelvin to Celsius for logic */
    int celsius = (temp_dk - 2731) / 10;

    /* Fan control logic */
    if (celsius < 50)
        new_pwm = 0;
    else if (celsius < 60)
        new_pwm = 75;
    else if (celsius < 67)
        new_pwm = 125;
    /* ... more levels ... */

    /* Set PWM via BCM2712 */
    bcm2712_set_pwm_duty(3, new_pwm);
}
```

---

## 4. Temperature Unit Reference

### Common FreeBSD Conversions

```c
/* 0°C in different units */
#define TZ_ZEROC       2731      /* deci-Kelvin */
#define CELSIUS_BASE   0         /* Obviously */
#define MILLIC_BASE    0         /* Obviously */
#define DECI_C_BASE    0         /* Obviously */
#define KELVIN_BASE    273       /* Approximately */

/* Conversion functions */
static inline int millic_to_celsius(int millic) {
    return millic / 1000;
}

static inline int millic_to_deci_celsius(int millic) {
    return millic / 100;
}

static inline int millic_to_deci_kelvin(int millic) {
    return (millic / 100) + TZ_ZEROC;
}

static inline int celsius_to_millic(int celsius) {
    return celsius * 1000;
}

static inline int deci_celsius_to_millic(int deci_c) {
    return deci_c * 100;
}

static inline int deci_kelvin_to_celsius(int deci_k) {
    return (deci_k - TZ_ZEROC) / 10;
}
```

---

## 5. Hardware Register Layout Example

For BCM2712, the thermal sensor registers might look like:

```c
/* Hypothetical BCM2712 thermal register definitions */

#define BCM2712_THERMAL_BASE        0x7EF00000
#define BCM2712_THERMAL_TEMP_REG    0x00

/* Register bit fields */
#define BCM2712_THERMAL_TEMP_VALID  (1 << 31)
#define BCM2712_THERMAL_TEMP_SHIFT  16
#define BCM2712_THERMAL_TEMP_MASK   0x3ff

/* Calibration constants (from datasheet) */
#define BCM2712_TEMP_BASE           1000    /* Base raw value @ 0°C */
#define BCM2712_TEMP_SCALE          1000    /* Scaling factor */
#define BCM2712_TEMP_DIV            1000    /* Divisor */

/* Alternative: Look up actual BCM2712 datasheet for real values */
```

---

## 6. Integration Checklist

- [ ] Define temperature constants and register offsets
- [ ] Implement `bcm2712_read_cpu_temp_millic()` based on hardware
- [ ] Add thermal structure to driver softc
- [ ] Initialize mutex and callout in attach
- [ ] Register sysctl handler
- [ ] Drain callout in detach
- [ ] Test with: `sysctl hw.bcm2712.temperature`
- [ ] Verify format with: `sysctl -d hw.bcm2712.temperature`

---

## 7. Testing and Verification

### Manual Testing

```bash
# Check if sysctl is registered
sysctl hw.bcm2712.temperature

# Monitor updates
watch -n 1 'sysctl hw.bcm2712.temperature'

# Get description
sysctl -d hw.bcm2712.temperature

# Expected output format
hw.bcm2712.temperature: 2731 (which is 0°C)
```

### Kernel Debug Output

```c
/* Add debug logging during development */
device_printf(dev, "Temperature: %d mC = %d dK\n",
              millic, bcm2712_millic_to_deci_kelvin(millic));
```

---

## 8. Device Tree Thermal Zone (Future Enhancement)

To support device tree thermal zones:

```dts
/ {
    thermal-zones {
        rpi5_thermal: rpi5-thermal {
            polling-delay-passive = <250>;
            polling-delay = <500>;
            thermal-sensors = <&bcm2712 0>;

            trips {
                cpu_warm: cpu-warm {
                    temperature = <60000>;      /* 60°C in milliCelsius */
                    hysteresis = <2000>;
                    type = "passive";
                };

                cpu_hot: cpu-hot {
                    temperature = <75000>;      /* 75°C in milliCelsius */
                    hysteresis = <5000>;
                    type = "critical";
                };
            };

            cooling-maps {
                map0 {
                    trip = <&cpu_warm>;
                    cooling-device = <&rpi5 0 THERMAL_NO_LIMIT>;
                };
            };
        };
    };
};
```

---

## 9. Common Pitfalls and Solutions

| Problem | Cause | Solution |
|---------|-------|----------|
| sysctl shows garbage values | Not converting units correctly | Double-check conversion formula + TZ_ZEROC |
| Temperature never updates | Callout not scheduled or drained | Verify `callout_reset()` in attach and `callout_drain()` in detach |
| Sysctl entry missing | Not registered | Check `device_get_sysctl_ctx()` and `SYSCTL_ADD_PROC()` |
| Temperature stuck at boot value | No periodic updates | Add interval to `callout_reset()` third parameter |
| Mutex deadlock | Locking in both handler and update | Use consistent locking order, avoid recursive locks |
| Module unload fails | Callout still active | Must drain callout before destroying mutex |

---

## 10. Reference Implementation Locations

| Component | Real Driver | File |
|-----------|-------------|------|
| Periodic update template | Armada | `/usr/src/sys/arm/mv/armada/thermal.c` |
| On-demand template | IMX ANATOP | `/usr/src/sys/arm/freescale/imx/imx6_anatop.c` |
| Multi-sensor handling | Allwinner | `/usr/src/sys/arm/allwinner/aw_thermal.c` |
| sysctl.9 format spec | Manual | `/usr/src/share/man/man9/sysctl.9` |

