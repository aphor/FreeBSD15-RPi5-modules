# FreeBSD Kernel Thermal Sensor Framework Research

## Executive Summary

FreeBSD kernel modules expose hardware temperature sensors through **sysctl** interface patterns, with NO unified thermal(9) framework. Temperature values are standardized as **deci-Kelvin (tenths of Kelvin)** in sysctl or **milli-Celsius** in device tree thermal zones. Drivers read raw hardware values and expose them via custom sysctl handlers.

---

## 1. Temperature Value Formats in FreeBSD

### 1.1 Sysctl Format Specifications

FreeBSD sysctl(9) defines format strings for temperature display:

**Format String: `IK[n]`**
- `IK` = Integer Kelvin
- Optional digit `n` = power of 10 scaling factor
  - `IK0` = Kelvin (0°C = 273 K)
  - `IK1` (or just `IK`) = **deciKelvin** (tenths of Kelvin) - **DEFAULT**
  - `IK3` = milliKelvin (thousandths of Kelvin)

**Source:** `/usr/src/share/man/man9/sysctl.9`, lines 584-587

**Example from sysctl(9) manual:**
```
IK [n]  temperature in Kelvin, multiplied by an optional single digit
        power of ten scaling factor: 1 (default) gives deciKelvin, 0 gives Kelvin, 3
        gives milliKelvin
```

### 1.2 deci-Kelvin to Celsius Conversion

The constant `TZ_ZEROC = 2731` is used across FreeBSD drivers:
- 0°C = 273.15 K ≈ 2731 in deciKelvin (2730 actual + 1 rounding)
- Conversions: `celsius_value = (deci_kelvin_value - 2731) / 10`

**Real driver example** (`/usr/src/sys/arm/freescale/imx/imx6_anatop.c`, line 141):
```c
#define TZ_ZEROC 2731  /* deci-Kelvin <-> deci-Celsius offset. */
```

---

## 2. FreeBSD Thermal Driver Patterns

FreeBSD has **NO unified thermal(9) consumer framework**. Each driver implements its own approach:

### 2.1 Pattern: Sysctl Handler with Read-on-Access (Allwinner, Armada)

**Used by:** Allwinner (`aw_thermal.c`), Armada (`thermal.c`)

**Key APIs:**

```c
/* Handler reads temperature and converts to Kelvin on each access */
static int
aw_thermal_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct aw_thermal_softc *sc;
    int sensor, val;

    sc = arg1;
    sensor = arg2;

    val = aw_thermal_gettemp(sc, sensor) + TEMP_C_TO_K;
    return sysctl_handle_opaque(oidp, &val, sizeof(val), req);
}

/* Register with SYSCTL_ADD_PROC */
SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
    OID_AUTO, sc->conf->sensors[i].name,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_NEEDGIANT,
    sc, i, aw_thermal_sysctl, "IK0",  /* IK0 = Kelvin */
    sc->conf->sensors[i].desc);
```

**Source:** `/usr/src/sys/arm/allwinner/aw_thermal.c`, lines 493-504, 671-676

**Key Points:**
- Converts to Kelvin **on each read** (live reads)
- `sysctl_handle_opaque()` for raw data passing
- Format string `"IK0"` = temperature in Kelvin (0°C = 273 K)

---

### 2.2 Pattern: Periodic Update with Callout (Armada, IMX)

**Used by:** Armada, IMX thermal drivers

**Key APIs:**

```c
/* Background callout updates cached temperature */
static void
armada_temp_update(void *arg)
{
    struct armada_thermal_softc *sc = arg;
    (void)armada_tsen_get_temp(sc, &sc->chip_temperature);
    callout_reset(&sc->temp_upd, hz, armada_temp_update, sc);
}

/* Sysctl handler returns cached value */
sctx = device_get_sysctl_ctx(dev);
schildren = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));
SYSCTL_ADD_LONG(sctx, schildren, OID_AUTO, "temperature",
    CTLFLAG_RD, &sc->chip_temperature, "SoC temperature");
```

**Source:** `/usr/src/sys/arm/mv/armada/thermal.c`, lines 206-209, 300-309

**Key Points:**
- Background task updates temperature every 1 second (`callout_reset(&sc->temp_upd, hz, ...)`)
- Direct sysctl variable exposure with `SYSCTL_ADD_LONG()`
- Returns cached value (efficient, not real-time)
- Uses `u_long` data type (platform-dependent size)

---

### 2.3 Pattern: Sysctl Handler with Conversion (IMX)

**Used by:** IMX (`imx6_anatop.c`)

**Key APIs:**

```c
/* Temperature read function */
static int
temp_sysctl_handler(SYSCTL_HANDLER_ARGS)
{
    struct imx6_anatop_softc *sc = arg1;
    uint32_t t;

    temp_update_count(sc);  /* Read hardware register */
    t = temp_from_count(sc, sc->temp_last_cnt) + TZ_ZEROC;
    return (sysctl_handle_int(oidp, &t, 0, req));
}

/* Register as sysctl proc */
SYSCTL_ADD_PROC(NULL, SYSCTL_STATIC_CHILDREN(_hw_imx),
    OID_AUTO, "temperature",
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
    sc, 0, temp_sysctl_handler, "IK",  /* IK = deciKelvin */
    "Current die temperature");
```

**Source:** `/usr/src/sys/arm/freescale/imx/imx6_anatop.c`, lines 493-504, 629-632

**Key Points:**
- Reads hardware on each sysctl access
- Converts hardware counts → degrees Celsius → deciKelvin
- Formula: `temp = temp_from_count(sc, raw_count) + TZ_ZEROC`
- `TZ_ZEROC = 2731` converts from deci-Celsius to deciKelvin
- Format string `"IK"` = deciKelvin (default scaling)

---

### 2.4 Pattern: PowerMac Thermal Subsystem (Callback-based)

**Used by:** PowerMac (`powermac_thermal.c`)

**Key APIs:**

```c
/* Sensor structure with callback */
struct pmac_therm {
    int target_temp, max_temp;  /* Tenths of a degree K */
    char name[32];
    int zone;
    int (*read)(struct pmac_therm *);  /* Callback */
};

/* Central management thread reads all sensors */
static void
pmac_therm_manage_fans(void)
{
    SLIST_FOREACH(sensor, &sensors, entries) {
        temp = sensor->sensor->read(sensor->sensor);  /* Callback */
        if (temp > 0)
            sensor->last_val = temp;
    }
}

/* Register sensor */
void
pmac_thermal_sensor_register(struct pmac_therm *sensor)
{
    struct pmac_sens_le *list_entry;
    list_entry = malloc(..., M_PMACTHERM, M_ZERO | M_WAITOK);
    list_entry->sensor = sensor;
    SLIST_INSERT_HEAD(&sensors, list_entry, entries);
}
```

**Source:** `/usr/src/sys/powerpc/powermac/powermac_thermal.c` and `.h`

**Key Points:**
- No sysctl interface (internal only)
- Driver-specific callback registration
- Central management thread polls all sensors
- Temperatures in tenths of Kelvin
- Used for automatic fan control

---

## 3. Hardware Temperature Reading Patterns

### 3.1 Common Register Access Macros

**Available macros in most ARM drivers:**

```c
RD4(sc, offset)           /* Read 32-bit register */
WR4(sc, offset, value)    /* Write 32-bit register */
bus_read_4(res, offset)   /* Alternative: read from bus resource */
bus_write_4(res, offset)  /* Alternative: write to bus resource */
```

### 3.2 Typical Sensor Reading Flow

```c
static int
read_temperature(struct driver_softc *sc)
{
    uint32_t raw_val;
    int celsius;

    /* 1. Read raw hardware register */
    raw_val = RD4(sc, TEMP_REGISTER_OFFSET);

    /* 2. Extract relevant bits */
    raw_val = (raw_val >> TEMP_SHIFT) & TEMP_MASK;

    /* 3. Check validity if supported */
    if (!is_valid(raw_val))
        return (EIO);

    /* 4. Convert to celsius using sensor calibration */
    celsius = hardware_to_celsius(raw_val);

    /* 5. Return in desired format (typically deci-Kelvin) */
    return (celsius * 10) + TZ_ZEROC;
}
```

---

## 4. Device Tree Thermal Zone Integration

FreeBSD **can parse but doesn't enforce** Linux device tree thermal zones. Drivers may optionally implement thermal zone support.

### 4.1 Standard Device Tree Thermal Zone Format

**Example:** `/usr/src/sys/contrib/device-tree/src/arm64/ti/k3-am62-thermal.dtsi`

```dts
thermal_zones: thermal-zones {
    main0_thermal: main0-thermal {
        polling-delay-passive = <250>;    /* milliSeconds */
        polling-delay = <500>;            /* milliSeconds */
        thermal-sensors = <&wkup_vtm0 0>;

        trips {
            main0_alert: main0-alert {
                temperature = <95000>;    /* milliCelsius */
                hysteresis = <2000>;      /* milliCelsius */
                type = "passive";
            };

            main0_crit: main0-crit {
                temperature = <105000>;   /* milliCelsius */
                hysteresis = <2000>;
                type = "critical";
            };
        };

        cooling-maps {
            map0 {
                trip = <&main0_alert>;
                cooling-device = <&cpu0 THERMAL_NO_LIMIT THERMAL_NO_LIMIT>;
            };
        };
    };
};
```

**Key Properties:**
- `thermal-sensors`: phandle to sensor device and index
- `temperature`: thresholds in **milliCelsius** (1000 = 1°C)
- `hysteresis`: deadband in **milliCelsius**
- `type`: "passive" (throttling), "critical" (shutdown), "active" (fan control)
- `cooling-device`: phandle to cooler device (CPU, fan, etc.)

### 4.2 Sensor Provider Interface (Linux-style)

```dts
thermal-sensor {
    compatible = "vendor,thermal-sensor";
    #thermal-sensor-cells = <1>;
    /* 0 = cpu temp, 1 = gpu temp, etc. */
};
```

**Current FreeBSD Status:**
- Device tree parsing works
- Thermal framework enforcement is **NOT implemented**
- Drivers can read thermal zones but must implement own logic

---

## 5. Real Driver Examples and Implementation Details

### 5.1 Allwinner Thermal Driver (`aw_thermal.c`)

**Temperature Conversion Formula:**

```c
static int
a83t_to_temp(uint32_t val, int sensor)
{
    return ((A83T_TEMP_BASE - (val * A83T_TEMP_MUL)) / A83T_TEMP_DIV);
}

#define A83T_TEMP_BASE    2719000   /* Base value */
#define A83T_TEMP_MUL     1000      /* Multiplier */
#define A83T_TEMP_DIV     14186     /* Divisor */
```

**Sysctl Registration:**

```c
for (i = 0; i < sc->conf->nsensors; i++)
    SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
        SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO, sc->conf->sensors[i].name,
        CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_NEEDGIANT,
        sc, i, aw_thermal_sysctl, "IK0",
        sc->conf->sensors[i].desc);
```

**File:** `/usr/src/sys/arm/allwinner/aw_thermal.c`

### 5.2 Armada Thermal Driver (`thermal.c`)

**Temperature Calculation:**

```c
static int
armada_tsen_get_temp(struct armada_thermal_softc *sc, u_long *temp)
{
    uint32_t reg;
    u_long tmp;
    u_long m, b, div;

    tdata = sc->tdata;

    /* Check validity */
    if ((tdata->is_valid != NULL) && !tdata->is_valid(sc))
        return (EIO);

    /* Extract bits */
    reg = bus_read_4(sc->stat_res, 0);
    reg = (reg >> tdata->temp_shift) & tdata->temp_mask;

    /* Apply formula: temp = (b - m*reg) / div  or  (m*reg - b) / div */
    b = tdata->coef_b;
    m = tdata->coef_m;
    div = tdata->coef_div;

    if (tdata->inverted)
        tmp = ((m * reg) - b) / div;
    else
        tmp = (b - (m * reg)) / div;

    *temp = READOUT_TO_C(tmp);  /* Convert to Celsius */
    return (0);
}

#define READOUT_TO_C(temp) ((temp) / 1000)  /* mK → °C */
```

**Periodic Update:**

```c
callout_init_mtx(&sc->temp_upd, &sc->temp_upd_mtx, 0);
callout_reset(&sc->temp_upd, hz, armada_temp_update, sc);  /* 1 second */
```

**File:** `/usr/src/sys/arm/mv/armada/thermal.c`

### 5.3 IMX ANATOP Driver (`imx6_anatop.c`)

**Key Constants:**

```c
#define TZ_ZEROC 2731               /* deci-Kelvin offset for 0°C */
#define IMX6_ANALOG_TEMPMON_TEMPSENSE0              0x180
#define IMX6_ANALOG_TEMPMON_TEMPSENSE0_VALID        (1 << 31)
#define IMX6_ANALOG_TEMPMON_TEMPSENSE0_TEMP_CNT_MASK   0xfff
#define IMX6_ANALOG_TEMPMON_TEMPSENSE0_TEMP_CNT_SHIFT  0
```

**Temperature Reading:**

```c
static void
temp_update_count(struct imx6_anatop_softc *sc)
{
    uint32_t val;

    val = imx6_anatop_read_4(IMX6_ANALOG_TEMPMON_TEMPSENSE0);
    if (!(val & IMX6_ANALOG_TEMPMON_TEMPSENSE0_VALID))
        return;

    sc->temp_last_cnt = (val & IMX6_ANALOG_TEMPMON_TEMPSENSE0_TEMP_CNT_MASK) >>
                        IMX6_ANALOG_TEMPMON_TEMPSENSE0_TEMP_CNT_SHIFT;
}

static int
temp_sysctl_handler(SYSCTL_HANDLER_ARGS)
{
    struct imx6_anatop_softc *sc = arg1;
    uint32_t t;

    temp_update_count(sc);
    t = temp_from_count(sc, sc->temp_last_cnt) + TZ_ZEROC;
    return (sysctl_handle_int(oidp, &t, 0, req));
}
```

**Sysctl Registration:**

```c
SYSCTL_ADD_PROC(NULL, SYSCTL_STATIC_CHILDREN(_hw_imx),
    OID_AUTO, "temperature",
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
    sc, 0, temp_sysctl_handler, "IK",
    "Current die temperature");
```

**File:** `/usr/src/sys/arm/freescale/imx/imx6_anatop.c`

---

## 6. ACPI Thermal Zone Driver (`acpi_thermal.c`)

**Sysctl Handler Pattern:**

```c
static int
acpi_tz_temp_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct acpi_tz_softc *sc;
    int temp, *temp_ptr;
    int error;

    sc = oidp->oid_arg1;
    temp_ptr = (int *)(void *)(uintptr_t)((uintptr_t)sc + oidp->oid_arg2);
    temp = *temp_ptr;

    error = sysctl_handle_int(oidp, &temp, 0, req);

    /* Error or no new value */
    if (error != 0 || req->newptr == NULL)
        return (error);

    /* Handle write - validate new value */
    acpi_tz_sanity(sc, &temp, "user-supplied temp");
    if (temp == -1)
        return (EINVAL);

    *temp_ptr = temp;
    return (0);
}
```

**Sysctl Registrations:**

```c
SYSCTL_ADD_PROC(&sc->tz_sysctl_ctx, SYSCTL_CHILDREN(sc->tz_sysctl_tree),
    OID_AUTO, "temperature", CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
    &sc->tz_temperature, 0, sysctl_handle_int, "IK",
    "current thermal zone temperature");

SYSCTL_ADD_PROC(&sc->tz_sysctl_ctx, SYSCTL_CHILDREN(sc->tz_sysctl_tree),
    OID_AUTO, "passive_cooling", 0, acpi_tz_passive_sysctl, "I",
    "cooling is active");
```

**File:** `/usr/src/sys/dev/acpica/acpi_thermal.c`

---

## 7. Sysctl Handler Implementation Patterns

### 7.1 Handler Signature

```c
static int
temperature_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct driver_softc *sc = arg1;  /* arg1 from SYSCTL_ADD_PROC */
    int sensor = arg2;                /* arg2 from SYSCTL_ADD_PROC */
    int temp_value;
    int error;

    /* Read current temperature */
    temp_value = read_hardware_temp(sc, sensor);

    /* Use handler to process the value */
    error = sysctl_handle_int(oidp, &temp_value, 0, req);

    if (error != 0 || req->newptr == NULL)
        return (error);

    /* If write requested, validate and update */
    if (validate_temp(temp_value)) {
        update_hardware_threshold(sc, sensor, temp_value);
        return (0);
    }

    return (EINVAL);
}
```

### 7.2 Handlers Available

```c
sysctl_handle_int(oidp, &val, 0, req)        /* Integer */
sysctl_handle_long(oidp, &val, 0, req)       /* Long */
sysctl_handle_opaque(oidp, &val, sizeof, req) /* Raw bytes */
sysctl_handle_string(oidp, buf, len, req)    /* String */
```

---

## 8. Sysctl Registration Macros

### 8.1 Common Patterns

```c
/* Dynamic registration (recommended) */
SYSCTL_ADD_PROC(
    device_get_sysctl_ctx(dev),        /* sysctl context */
    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), /* parent */
    OID_AUTO,                           /* auto-assign OID */
    "temperature",                      /* sysctl name */
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,  /* flags */
    sc,                                 /* arg1: softc pointer */
    0,                                  /* arg2: sensor index */
    temperature_sysctl,                 /* handler function */
    "IK",                               /* format: deciKelvin */
    "Current CPU temperature"           /* description */
);

/* Static registration */
SYSCTL_PROC(_hw_mydriver, OID_AUTO, "temperature",
    CTLTYPE_INT | CTLFLAG_RD, arg1, arg2,
    temperature_sysctl, "IK", "description");

/* Direct variable exposure */
SYSCTL_ADD_INT(
    device_get_sysctl_ctx(dev),
    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
    OID_AUTO, "temperature",
    CTLFLAG_RD,
    &sc->cached_temp,
    0,
    "Current temperature"
);
```

---

## 9. Key Takeaways for Implementation

### Temperature Unit Standards
1. **Device Tree (Linux style)**: milliCelsius (1000 = 1°C)
2. **FreeBSD sysctl default**: deciKelvin (2731 = 0°C)
3. **Internal storage**: varies by driver (raw counts, deci-Celsius, etc.)

### Recommended Implementation Pattern
```c
/* In driver softc */
struct driver_softc {
    int cached_temp;           /* deci-Kelvin format */
    struct callout update;     /* periodic update */
};

/* Background update task */
static void
temp_update_callout(void *arg)
{
    struct driver_softc *sc = arg;
    int raw = read_hardware_register(sc);
    sc->cached_temp = convert_to_deci_kelvin(raw);
    callout_reset(&sc->update, hz, temp_update_callout, sc);
}

/* Sysctl handler */
static int
temp_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct driver_softc *sc = arg1;
    return (sysctl_handle_int(oidp, &sc->cached_temp, 0, req));
}

/* Registration */
SYSCTL_ADD_PROC(ctx, parent, OID_AUTO, "temperature",
    CTLTYPE_INT | CTLFLAG_RD, sc, 0,
    temp_sysctl, "IK", "CPU temperature");
```

### Critical Conversions
```c
#define TZ_ZEROC 2731           /* deci-K offset for 0°C */

int to_deci_kelvin(int celsius) {
    return (celsius * 10) + TZ_ZEROC;
}

int to_celsius(int deci_kelvin) {
    return (deci_kelvin - TZ_ZEROC) / 10;
}
```

---

## 10. References and File Locations

| Component | File Path | Key Functions |
|-----------|-----------|---|
| **Sysctl Format Spec** | `/usr/src/share/man/man9/sysctl.9` | Format codes (IK, IK0, IK3) |
| **Allwinner Driver** | `/usr/src/sys/arm/allwinner/aw_thermal.c` | `aw_thermal_sysctl()`, `aw_thermal_gettemp()` |
| **Armada Driver** | `/usr/src/sys/arm/mv/armada/thermal.c` | `armada_tsen_get_temp()`, `armada_temp_update()` |
| **IMX ANATOP Driver** | `/usr/src/sys/arm/freescale/imx/imx6_anatop.c` | `temp_sysctl_handler()`, `temp_update_count()` |
| **PowerMac Thermal** | `/usr/src/sys/powerpc/powermac/powermac_thermal.c` | `pmac_thermal_sensor_register()` |
| **ACPI Thermal** | `/usr/src/sys/dev/acpica/acpi_thermal.c` | `acpi_tz_temp_sysctl()` |
| **Device Tree Examples** | `/usr/src/sys/contrib/device-tree/src/` | Thermal zone binding examples |

---

## Summary Table: Driver Comparison

| Driver | Architecture | Approach | Update | Format | Register Access |
|--------|--------------|----------|--------|--------|------------------|
| aw_thermal | ARM32 | Handler | On-demand | IK0 (K) | RD4/WR4 |
| armada | ARM32 | Cached + Callout | 1 sec | Direct var | bus_read_4 |
| imx6_anatop | ARM32 | Handler | On-demand | IK (dK) | imx6_read_4 |
| powermac | PPC64 | Callback | Poll thread | dK (internal) | Custom |
| acpi_thermal | x86/x64 | Handler | On-demand | IK (dK) | ACPI methods |
