# FreeBSD Kernel Thermal APIs - Quick Reference

## Essential Include Files

```c
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>      /* sysctl macros and functions */
#include <sys/kernel.h>      /* SYSINIT, etc. */
#include <sys/bus.h>         /* device_t, bus macros */
#include <sys/rman.h>        /* resource management */
#include <sys/mutex.h>       /* mtx_* synchronization */
#include <machine/bus.h>     /* RD4, WR4 macros */
```

---

## Hardware Register Access

### Read/Write Register Macros

Available if `<machine/bus.h>` and resource_h setup is done:

```c
/* Define resource handle in softc */
struct driver_softc {
    struct resource *res;
    bus_space_tag_t     bst;
    bus_space_handle_t  bsh;
};

/* Macro-style access (preferred in device drivers) */
#define RD4(sc, reg)    bus_read_4((sc)->res, (reg))
#define WR4(sc, reg, v) bus_write_4((sc)->res, (reg), (v))
#define RD1(sc, reg)    bus_read_1((sc)->res, (reg))
#define WR1(sc, reg, v) bus_write_1((sc)->res, (reg), (v))

/* Direct function calls */
uint32_t bus_read_4(struct resource *res, int offset);
void bus_write_4(struct resource *res, int offset, uint32_t value);
uint16_t bus_read_2(struct resource *res, int offset);
void bus_write_2(struct resource *res, int offset, uint16_t value);
uint8_t bus_read_1(struct resource *res, int offset);
void bus_write_1(struct resource *res, int offset, uint8_t value);
```

### Delay Functions

```c
#include <sys/systm.h>

DELAY(microseconds);  /* Sleep for microseconds (busy-wait) */
pause("description", ticks);  /* Sleep for ticks (1/hz seconds) */
```

---

## Mutex Synchronization

```c
#include <sys/mutex.h>

/* Define in softc */
struct mtx temp_mtx;

/* Initialization */
mtx_init(&sc->temp_mtx, "description", NULL, MTX_DEF);

/* Locking */
mtx_lock(&sc->temp_mtx);
mtx_unlock(&sc->temp_mtx);

/* Assertion */
mtx_assert(&sc->temp_mtx, MA_OWNED);

/* Cleanup */
mtx_destroy(&sc->temp_mtx);
```

---

## Callout/Timer Interface

```c
#include <sys/callout.h>

/* Define in softc */
struct callout temp_update;

/* Initialization (before use) */
callout_init(&sc->temp_update, CALLOUT_MPSAFE);  /* General init */
callout_init_mtx(&sc->temp_update, &sc->temp_mtx, 0);  /* With mutex */

/* Schedule callback (called periodically) */
callout_reset(&sc->temp_update, hz, callback_func, arg);
/*
 * hz = 1 second interval (system tick frequency)
 * hz/2 = 500 milliseconds
 * hz*2 = 2 seconds
 */

/* Drain/Cancel (must call before destroy/detach) */
callout_drain(&sc->temp_update);  /* Block until callback completes */
callout_stop(&sc->temp_update);   /* Cancel without blocking */

/* Cleanup */
callout_drain(&sc->temp_update);  /* Must be done before rmmod */
```

---

## Sysctl Interface

### Registration Macros

```c
#include <sys/sysctl.h>

/* Get sysctl context and tree from device */
struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
struct sysctl_oid_list *parent = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));

/* Register integer variable (read-only) */
SYSCTL_ADD_INT(ctx, parent, OID_AUTO, "varname",
    CTLFLAG_RD, &variable, 0, "Description");

/* Register integer with handler (read-write, computed values) */
SYSCTL_ADD_PROC(ctx, parent, OID_AUTO, "temperature",
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
    arg1_softc, arg2_index, handler_function, "IK", "Description");

/* Register long variable */
SYSCTL_ADD_LONG(ctx, parent, OID_AUTO, "varname",
    CTLFLAG_RD, &variable, "Description");

/* Register opaque/struct (raw bytes) */
SYSCTL_ADD_OPAQUE(ctx, parent, OID_AUTO, "varname",
    CTLFLAG_RD, ptr, size, "format", "Description");

/* Register node for grouping */
SYSCTL_ADD_NODE(ctx, parent, OID_AUTO, "nodename",
    CTLFLAG_RW, 0, "Description");
```

### Sysctl Handler Signature

```c
static int
temperature_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct driver_softc *sc = arg1;
    int sensor_index = arg2;
    int temp_value;
    int error;

    /* Fetch value (read current temperature) */
    temp_value = read_temperature(sc, sensor_index);

    /* Call appropriate handler based on type */
    error = sysctl_handle_int(oidp, &temp_value, 0, req);

    /* If error or no write request, return */
    if (error != 0 || req->newptr == NULL)
        return (error);

    /* If write was requested, validate and apply */
    if (validate_value(temp_value)) {
        update_hardware(sc, sensor_index, temp_value);
        return (0);
    }

    return (EINVAL);  /* Invalid value */
}
```

### Sysctl Handlers

```c
/* Integer value */
int sysctl_handle_int(SYSCTL_HANDLER_ARGS);

/* Long value */
int sysctl_handle_long(SYSCTL_HANDLER_ARGS);

/* Opaque/raw bytes */
int sysctl_handle_opaque(SYSCTL_HANDLER_ARGS);

/* String value */
int sysctl_handle_string(SYSCTL_HANDLER_ARGS);

/* Quad (64-bit) */
int sysctl_handle_quad(SYSCTL_HANDLER_ARGS);
```

---

## Format Strings for Sysctl

### Temperature Formats (Most Common)

```c
/* Temperature format codes */
"IK"        /* Integer Kelvin (deciKelvin default) */
            /* Display: 0°C = 2731 dK */
            /* User sees: 27.31 K after conversion */

"IK0"       /* Integer Kelvin (raw Kelvin) */
            /* Display: 0°C = 273 K */

"IK3"       /* Integer Kelvin (milliKelvin) */
            /* Display: 0°C = 273150 mK */

"I"         /* Plain integer (no format hint) */
"IU"        /* Unsigned integer */
"L"         /* Long integer */
"A"         /* char * (string) */
"N"         /* Node (group) */
```

---

## Device Attachment Lifecycle

### Probe Phase

```c
static int
driver_probe(device_t dev)
{
    /* Check if this driver should handle the device */
    if (is_compatible(dev)) {
        device_set_desc(dev, "Device Description");
        return (BUS_PROBE_DEFAULT);  /* or specific priority */
    }
    return (ENXIO);  /* Not compatible */
}
```

### Attach Phase

```c
static int
driver_attach(device_t dev)
{
    struct driver_softc *sc = device_get_softc(dev);

    /* 1. Allocate resources */
    sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
    if (sc->res == NULL)
        goto fail;

    /* 2. Initialize driver-specific structures */
    mtx_init(&sc->temp_mtx, "driver", NULL, MTX_DEF);
    callout_init_mtx(&sc->temp_update, &sc->temp_mtx, 0);

    /* 3. Perform initial hardware setup */
    hardware_init(sc);

    /* 4. Start periodic tasks */
    callout_reset(&sc->temp_update, hz, update_callback, sc);

    /* 5. Register sysctl interfaces */
    register_sysctl(dev, sc);

    return (0);

fail:
    driver_detach(dev);
    return (ENXIO);
}
```

### Detach Phase

```c
static int
driver_detach(device_t dev)
{
    struct driver_softc *sc = device_get_softc(dev);

    /* 1. Stop periodic tasks */
    callout_drain(&sc->temp_update);

    /* 2. Clean up hardware */
    hardware_shutdown(sc);

    /* 3. Destroy synchronization primitives */
    mtx_destroy(&sc->temp_mtx);

    /* 4. Release resources */
    if (sc->res != NULL) {
        bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);
        sc->res = NULL;
    }

    return (0);
}
```

---

## Complete Minimal Example

```c
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <machine/bus.h>

#define RD4(sc, reg)    bus_read_4((sc)->res, (reg))
#define TZ_ZEROC        2731

struct driver_softc {
    device_t dev;
    struct resource *res;
    int temp_current;
    struct mtx temp_mtx;
    struct callout temp_update;
};

static int
read_temp_from_hw(struct driver_softc *sc)
{
    uint32_t raw = RD4(sc, 0x00);
    return (raw >> 4) & 0xfff;  /* Extract bits [15:4] */
}

static void
temp_update_func(void *arg)
{
    struct driver_softc *sc = arg;
    int raw;

    mtx_lock(&sc->temp_mtx);
    raw = read_temp_from_hw(sc);
    sc->temp_current = (raw / 10) + TZ_ZEROC;  /* Convert to dK */
    mtx_unlock(&sc->temp_mtx);

    callout_reset(&sc->temp_update, hz, temp_update_func, sc);
}

static int
temp_sysctl_handler(SYSCTL_HANDLER_ARGS)
{
    struct driver_softc *sc = arg1;
    int temp;

    mtx_lock(&sc->temp_mtx);
    temp = sc->temp_current;
    mtx_unlock(&sc->temp_mtx);

    return (sysctl_handle_int(oidp, &temp, 0, req));
}

static int
driver_attach(device_t dev)
{
    struct driver_softc *sc = device_get_softc(dev);
    int rid = 0;

    sc->dev = dev;

    /* Allocate memory resource */
    sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
    if (sc->res == NULL)
        return (ENXIO);

    /* Initialize synchronization */
    mtx_init(&sc->temp_mtx, "thermal", NULL, MTX_DEF);
    callout_init_mtx(&sc->temp_update, &sc->temp_mtx, 0);

    /* Start updates */
    temp_update_func(sc);

    /* Register sysctl */
    SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
        SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO, "temperature",
        CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_MPSAFE,
        sc, 0, temp_sysctl_handler, "IK", "Temperature");

    return (0);
}

static int
driver_detach(device_t dev)
{
    struct driver_softc *sc = device_get_softc(dev);

    callout_drain(&sc->temp_update);
    mtx_destroy(&sc->temp_mtx);

    if (sc->res != NULL)
        bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);

    return (0);
}

static device_method_t driver_methods[] = {
    DEVMETHOD(device_attach, driver_attach),
    DEVMETHOD(device_detach, driver_detach),
    DEVMETHOD_END
};

static driver_t driver_driver = {
    "driver", driver_methods, sizeof(struct driver_softc)
};

DRIVER_MODULE(driver, simplebus, driver_driver, 0, 0);
```

---

## Common Error Codes

```c
#include <sys/errno.h>

ENXIO       /* Device not configured / not found */
EINVAL      /* Invalid argument / value out of range */
ERANGE      /* Numeric result out of range */
EIO         /* Input/output error (hardware error) */
EBUSY       /* Device busy */
ENOENT      /* No such file or directory */
EPERM       /* Operation not permitted */
EACCES      /* Permission denied */
EALREADY    /* Operation already in progress */
```

---

## Memory Allocation

```c
#include <sys/malloc.h>

/* Define malloc bucket (in module) */
static MALLOC_DEFINE(M_DRIVER, "driver", "Driver memory");

/* Allocate memory */
void *ptr = malloc(size, M_DRIVER, M_WAITOK | M_ZERO);

/* Free memory */
free(ptr, M_DRIVER);

/* Allocation flags */
M_WAITOK    /* Block if necessary */
M_NOWAIT    /* Return NULL if can't allocate immediately */
M_ZERO      /* Initialize allocated memory to zero */
```

---

## Device Tree / OFW Access

```c
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

/* Check device status */
if (!ofw_bus_status_okay(dev))
    return (ENXIO);

/* Check compatibility string */
if (ofw_bus_is_compatible(dev, "vendor,device-type"))
    return (BUS_PROBE_DEFAULT);

/* Get property */
phandle_t node = ofw_bus_get_node(dev);
```

---

## Debugging and Logging

```c
#include <sys/systm.h>

/* Print to console and syslog */
device_printf(dev, "Message: %d\n", value);
printf("Kernel message\n");

/* Kernel panic */
panic("Fatal error: %s\n", reason);

/* Boot messages (visible with bootverbose) */
if (bootverbose)
    device_printf(dev, "Debug: %d\n", value);
```

---

## Module Boilerplate

```c
#include <sys/module.h>

/* Device methods */
static device_method_t methods[] = {
    DEVMETHOD(device_probe, driver_probe),
    DEVMETHOD(device_attach, driver_attach),
    DEVMETHOD(device_detach, driver_detach),
    DEVMETHOD_END
};

/* Driver definition */
static driver_t driver = {
    "drivername",          /* Module name */
    methods,
    sizeof(struct driver_softc)
};

/* Module registration */
DRIVER_MODULE(drivername, parent_bus, driver, 0, 0);

/* Optional: Module dependency on other modules */
MODULE_DEPEND(drivername, bcm2712, 1, 1, 1);  /* Require bcm2712 */
```

---

## Temperature Unit Conversion Quick Table

| Unit | Value for 0°C | Value for 50°C | Notes |
|------|---------------|----------------|-------|
| Celsius | 0 | 50 | Base unit |
| milliCelsius (mC) | 0 | 50000 | Device tree standard |
| deci-Celsius (dC) | 0 | 500 | Intermediate |
| Kelvin | 273 | 323 | Absolute scale |
| deci-Kelvin (dK) | 2731 | 5231 | **sysctl standard** |
| milliKelvin (mK) | 273150 | 323150 | Rare |

**Key constant:** `TZ_ZEROC = 2731` (0°C in deci-Kelvin)

---

## Testing Commands

```bash
# Check if sysctl exists
sysctl hw.driver.temperature

# Monitor in real-time
watch -n 1 'sysctl hw.driver.temperature'

# Get description
sysctl -d hw.driver.temperature

# Raw value (in deci-Kelvin if using "IK" format)
sysctl -n hw.driver.temperature

# Set value (if writable)
sysctl hw.driver.temperature=2731

# View all driver sysctls
sysctl -a | grep hw.driver

# Check module load
kldstat | grep driver

# Load module
kldload driver

# Unload module
kldunload driver
```

---

## Key Files to Reference

| Task | File |
|------|------|
| sysctl(9) API | `/usr/src/share/man/man9/sysctl.9` |
| Allwinner driver | `/usr/src/sys/arm/allwinner/aw_thermal.c` |
| Armada driver | `/usr/src/sys/arm/mv/armada/thermal.c` |
| IMX driver | `/usr/src/sys/arm/freescale/imx/imx6_anatop.c` |
| Bus resource API | `/usr/src/share/man/man9/bus_read_4.9` |
| Device attach | `/usr/src/share/man/man9/DEVICE_ATTACH.9` |
| Mutex | `/usr/src/share/man/man9/mutex.9` |
| Callout | `/usr/src/share/man/man9/callout.9` |

