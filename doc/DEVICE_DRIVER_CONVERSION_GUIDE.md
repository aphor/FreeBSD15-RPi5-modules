# Converting BCM2712 to Device Driver for Hardware Register Access

## Overview

This guide shows how to convert the bcm2712 module to a proper FreeBSD device driver that can access the AVS thermal sensor hardware registers via `bus_space_map()`.

## High-Level Architecture

### Current: Module-Based
```
bcm2712.c (module) → global softc → cached temperature
                  ↖︎ no device tree attachment
                  ↖︎ no physical memory access
```

### Target: Device Driver
```
bcm2712.c (driver) ← device tree node → OFW probe
       ↓
bus_alloc_resource() → bus_space_map() → physical memory
       ↓
actual register reads → temperature value
```

## Required Changes

### 1. Add Device Tree Node

**File**: Create/modify device tree overlay for BCM2712

**Example** (`.dts` or overlay):
```dts
/ {
    /* Add to existing device tree */
    thermal_sensor {
        compatible = "raspberrypi,bcm2712-thermal";
        reg = <0x7d5d2200 0x4>;  /* Physical address and size */
        status = "okay";
    };
};
```

Or if part of existing SoC node:
```dts
&bcm2712 {
    thermal_sensor: thermal@7d5d2200 {
        compatible = "raspberrypi,bcm2712-thermal";
        reg = <0x7d5d2200 0x4>;
        status = "okay";
    };
};
```

### 2. Implement Device Driver Methods

**File**: Modify `bcm2712.c`

#### Add NewHeaders
```c
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
```

#### Replace Module Handler with Driver Methods

```c
/* Device driver probe */
static int
bcm2712_thermal_probe(device_t dev)
{
    if (!ofw_bus_is_compatible(dev, "raspberrypi,bcm2712-thermal"))
        return (ENXIO);

    device_set_desc(dev, "BCM2712 Thermal Sensor");
    return (BUS_PROBE_DEFAULT);
}

/* Device driver attach */
static int
bcm2712_thermal_attach(device_t dev)
{
    struct bcm2712_softc *sc = device_get_softc(dev);
    int error = 0;

    sc->dev = dev;

    /* Initialize mutexes */
    mtx_init(&sc->mtx, "bcm2712", NULL, MTX_DEF);
    mtx_init(&sc->thermal_mtx, "bcm2712_thermal", NULL, MTX_DEF);

    /* Allocate memory resource from device tree */
    sc->mem_rid = 0;
    sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
        &sc->mem_rid, RF_ACTIVE);

    if (sc->mem == NULL) {
        device_printf(dev, "Cannot allocate memory resource\n");
        error = ENXIO;
        goto fail;
    }

    /* Get bus space tag and handle from resource */
    sc->bst = rman_get_bustag(sc->mem);
    sc->bsh = rman_get_bushandle(sc->mem);

    if (sc->bst == NULL || sc->bsh == 0) {
        device_printf(dev, "Cannot get bus space from resource\n");
        error = ENXIO;
        goto fail;
    }

    /* Now we can use bus_space_read_4 to access hardware */
    printf("bcm2712: AVS thermal sensor mapped successfully\n");

    /* Initialize thermal structures */
    callout_init_mtx(&sc->thermal_callout, &sc->thermal_mtx, 0);
    sc->cached_temp_mc = 50000;  /* 50°C default */

    /* Initialize sysctl */
    sysctl_ctx_init(&sc->sysctl_ctx);
    /* ... sysctl registration code ... */

    /* Start periodic updates */
    mtx_lock(&sc->thermal_mtx);
    callout_reset(&sc->thermal_callout, hz, bcm2712_thermal_update, sc);
    mtx_unlock(&sc->thermal_mtx);

    return (0);

fail:
    if (sc->mem != NULL)
        bus_release_resource(dev, SYS_RES_MEMORY,
            sc->mem_rid, sc->mem);
    mtx_destroy(&sc->thermal_mtx);
    mtx_destroy(&sc->mtx);
    return (error);
}

/* Device driver detach */
static int
bcm2712_thermal_detach(device_t dev)
{
    struct bcm2712_softc *sc = device_get_softc(dev);

    mtx_lock(&sc->thermal_mtx);
    callout_drain(&sc->thermal_callout);
    mtx_unlock(&sc->thermal_mtx);

    sysctl_ctx_free(&sc->sysctl_ctx);

    if (sc->mem != NULL) {
        bus_release_resource(dev, SYS_RES_MEMORY,
            sc->mem_rid, sc->mem);
    }

    mtx_destroy(&sc->thermal_mtx);
    mtx_destroy(&sc->mtx);

    return (0);
}

/* Driver declaration */
static device_method_t bcm2712_thermal_methods[] = {
    DEVMETHOD(device_probe,   bcm2712_thermal_probe),
    DEVMETHOD(device_attach,  bcm2712_thermal_attach),
    DEVMETHOD(device_detach,  bcm2712_thermal_detach),
    DEVMETHOD_END
};

static driver_t bcm2712_thermal_driver = {
    "bcm2712_thermal",
    bcm2712_thermal_methods,
    sizeof(struct bcm2712_softc),
};

DRIVER_MODULE(bcm2712_thermal, simplebus, bcm2712_thermal_driver,
    NULL, NULL);
```

### 3. Update Register Reading Function

**In `bcm2712_thermal_read_raw()`**, replace the placeholder with actual hardware access:

**Before**:
```c
if (!sc->avs_mapped || sc->avs_vaddr == NULL) {
    return (sc->cached_temp_mc);
}
raw_value = *(volatile uint32_t *)(sc->avs_vaddr);
```

**After** (with device driver context):
```c
/* Now we have bus_space mappings from device tree */
uint32_t raw_value;

if (sc->mem == NULL) {
    device_printf(sc->dev, "AVS memory not mapped\n");
    return (sc->cached_temp_mc);
}

/* Read hardware register via bus_space */
raw_value = bus_space_read_4(sc->bst, sc->bsh, 0);

if (raw_value == 0xffffffff) {
    device_printf(sc->dev, "Failed to read AVS register\n");
    return (sc->cached_temp_mc);
}
```

### 4. Update bcm2712_var.h Structure

Keep the device_t and bus_space fields; remove avs_vaddr:

```c
struct bcm2712_softc {
    device_t dev;
    struct mtx mtx;

    /* Memory resources - now properly allocated by device framework */
    struct resource *mem;
    int mem_rid;
    bus_space_tag_t bst;
    bus_space_handle_t bsh;

    /* PWM channels (4 available on RP1) */
    struct bcm2712_pwm_channel channels[BCM2712_PWM_NCHANNELS];

    /* Thermal sensor support */
    struct mtx thermal_mtx;
    struct callout thermal_callout;
    uint32_t cached_temp_mc;
    time_t last_update;
    struct sysctl_ctx_list sysctl_ctx;
    struct sysctl_oid *sysctl_tree;

    /* AVS mapped successfully */
    int avs_mapped;
};
```

## Implementation Steps

### Step 1: Modify Device Tree Overlay
- Add thermal sensor node to FreeBSD RPi 5 device tree
- Device tree compiler will convert to DTB

### Step 2: Refactor bcm2712.c
- Replace module modevent with driver methods
- Implement probe, attach, detach
- Remove module-specific initialization code
- Update Makefile to use standard driver compilation

### Step 3: Update Makefiles
```makefile
# Makefile.bcm2712
KMOD=    bcm2712_thermal
SRCS=    bcm2712.c
SRCS+=   device_if.h bus_if.h ofw_bus_if.h opt_fdt.h opt_global.h

CFLAGS+= -I/usr/src/sys -I/usr/src/sys/contrib/libfdt \
         -I/usr/src/sys/dev/fdt -I/usr/src/sys/dev/pwm

.include <bsd.kmod.mk>
```

### Step 4: Test
```bash
# Device tree must be loaded first
# Build and install
make bcm2712
sudo make install

# Verify device tree includes thermal sensor node
dtc -I dtb -O dts /boot/dtb/bcm2712-rpi5.dtb | grep -A5 thermal

# Check if device attaches
dmesg | grep bcm2712

# Verify sysctl works
sysctl hw.bcm2712.thermal.cpu_temp
```

## PWM Module Consideration

The current bcm2712 module also handles PWM. In the device driver architecture:

**Option A**: Keep PWM in separate module (recommended)
- Thermal driver: Reads temperature via device tree
- PWM module: Provides abstract PWM interface
- rpi5 module: Uses both services

**Option B**: Integrate PWM into thermal driver
- Single driver handles both thermal and PWM
- More complex, but unified
- Requires PWM device tree nodes

## Verification Checklist

- [ ] Device tree node added to FreeBSD DTB
- [ ] bcm2712_thermal device attaches during boot
- [ ] `devinfo | grep bcm2712_thermal` shows device
- [ ] Register reads return valid values (not 0 or 0xffffffff)
- [ ] Temperature validity bits (0x0410) are set
- [ ] Converted temperature is in reasonable range (20-80°C)
- [ ] sysctl shows realistic temperature values
- [ ] Temperature changes with system load
- [ ] No kernel panics or SError interrupts

## Troubleshooting

### Device Won't Attach
1. Check device tree: `dtc -I dtb -O dts /boot/dtb/dtb | grep thermal`
2. Verify `compatible` string matches in code
3. Check physical address and size
4. Enable OFW debugging: `sysctl hw.ofw.dump_sysinfo=1`

### Register Reads Return 0
1. Verify physical address is correct (0x7d5d2200)
2. Check register validity bits aren't set (sensor not active)
3. Verify memory resource allocation succeeded
4. Check bus_space tag/handle are correct

### Temperature Values Wrong
1. Verify conversion formula: temp = (-550 * code) + 450000
2. Check raw code extracts bits 0-9 correctly
3. Verify deciKelvin offset: (mC / 100) + 2731
4. Compare with Raspbian reading via `/sys/class/thermal/thermal_zone0/temp`

## References

- **FreeBSD Device Driver Guide**: `/usr/src/share/doc/en_US.ISO8859-1/books/arch/`
- **OFW Bus Pattern**: `sys/dev/fdt/*` examples
- **Bus Space**: `sys/sys/bus.h`
- **Armada Thermal Driver**: `sys/arm/mv/armada/thermal.c` (excellent reference)

## Expected Behavior After Implementation

```
$ dmesg | grep bcm2712
bcm2712_thermal0: <BCM2712 Thermal Sensor> at mmio 0x7d5d2200
bcm2712: AVS thermal sensor mapped successfully

$ sysctl hw.bcm2712.thermal.cpu_temp
hw.bcm2712.thermal.cpu_temp: 45.2C

$ watch -n 1 'sysctl hw.bcm2712.thermal.cpu_temp'
# Shows temperature updating in real-time
```
