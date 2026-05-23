# ZFS Root Filesystem Support for RPI5 Kernel

## Problem

The custom RPI5 kernel failed to boot with error "unknown filesystem" at mountroot when trying to mount the ZFS root filesystem `zfs:dunn/ROOT/default`. The kernel loaded successfully but couldn't recognize ZFS as a valid filesystem type.

## Root Cause

The RPI5 kernel configuration was missing explicit ZFS support compilation. While the kernel inherits from GENERIC configuration, ZFS support is not automatically enabled - it must be explicitly configured via the kernel options.

## Solution

Added `options ZFS` to the RPI5 kernel configuration file at `/usr/src/sys/arm64/conf/RPI5`.

### Configuration Change

**File**: `/usr/src/sys/arm64/conf/RPI5`

```diff
+# ZFS Root Filesystem Support
+options 	ZFS
```

### Why This Works

1. **Kernel Option**: The `options ZFS` directive compiles ZFS filesystem support directly into the kernel
2. **Early Availability**: Having ZFS compiled in ensures it's available at boot time when mountroot is called
3. **Module Fallback**: The loader can still load the ZFS module via `loader.conf` for additional runtime features
4. **ARM64 Standard**: This matches the ARM64 NOTES configuration which documents ZFS as an available option

## Building the Fixed Kernel

```bash
cd /usr/src
sudo make -j$(sysctl -n hw.ncpu) KERNCONF=RPI5 buildkernel
sudo make KERNCONF=RPI5 installkernel
sudo reboot
```

## Verification After Boot

```bash
# Verify ZFS filesystem is recognized
mount | grep zfs

# Check ZFS pool status
zpool status

# Verify sysctl shows ZFS available
sysctl kern.features.zfs

# Check kernel config included ZFS
strings /boot/kernel/kernel | grep -i "options.*zfs" || echo "Checking boot messages..." && dmesg | grep -i zfs
```

## Expected Boot Sequence

1. **Bootloader** (U-Boot/Barebox): Loads FreeBSD kernel
2. **Kernel Load**: RPI5 kernel initializes, outputs:
   - CPU detected
   - Memory initialization
   - Device enumeration
3. **Module Loading**: Loader loads ZFS module via `zfs_load="YES"` from `/boot/loader.conf`
4. **Mountroot**: Kernel recognizes ZFS filesystem and mounts `zfs:dunn/ROOT/default` as root
5. **Filesystem Mount**: Root filesystem becomes available, init process starts

## Technical Details

### ZFS Kernel Configuration

The `options ZFS` configuration:
- Compiles ZFS filesystem driver into the kernel binary
- Makes ZFS a recognized filesystem type in the `VFS_LOOKUP` table
- Ensures ZFS support is available before modules are loaded
- Does not conflict with loadable ZFS module (module provides additional functionality)

### Boot Timeline

```
Bootloader (seconds)
         ↓
    Kernel Loads
         ↓
    ZFS Module Loaded (loader.conf: zfs_load="YES")
         ↓
    Mountroot Called
         ↓
    Kernel Recognizes ZFS ← KEY POINT (requires compiled ZFS support)
         ↓
    Root Filesystem Mounted (zfs:dunn/ROOT/default)
         ↓
    Init Process Starts
         ↓
    FreeBSD Boot Complete
```

## Architecture Decision

### Why Not Module-Only?

While ZFS could theoretically be loaded as a module before mountroot, this would require:
1. Modifying the bootloader to load ZFS module specifically
2. Complex boot sequencing logic
3. Module versioning must match kernel exactly

### Why Compile-In?

- Standard FreeBSD approach for critical filesystems (FFS, NFS)
- ZFS is the primary root filesystem on this system
- Clean boot process without early module loading complexity
- Ensures filesystem is available before any module-dependent operations

## Compatibility with GENERIC

The RPI5 configuration:
- Still inherits from GENERIC for maximum compatibility
- Adds only: `options ZFS` for filesystem support
- Adds only: `device bcm2712_thermal` for thermal sensor
- Everything else comes from GENERIC/std.* includes

## Files Modified

- `/usr/src/sys/arm64/conf/RPI5` - Added ZFS support option

## Build System Files (Already in Place)

- `/usr/src/sys/arm64/broadcom/bcm2712/bcm2712_thermal.c` - Thermal driver
- `/usr/src/sys/arm64/conf/std.broadcom` - Device definitions
- `/usr/src/sys/conf/files.arm64` - Build integration

## Troubleshooting

### If Still Getting "Unknown Filesystem"

1. **Verify config was added**:
   ```bash
   grep "^options.*ZFS" /usr/src/sys/arm64/conf/RPI5
   ```

2. **Check kernel was rebuilt**:
   ```bash
   sudo make KERNCONF=RPI5 clean
   sudo make -j$(sysctl -n hw.ncpu) KERNCONF=RPI5 buildkernel
   ```

3. **Verify module is available**:
   ```bash
   ls -la /boot/modules/zfs.ko
   ```

4. **Check loader.conf**:
   ```bash
   cat /boot/loader.conf | grep zfs_load
   ```

### If Boot Hangs at Mountroot

1. Interrupt boot (Ctrl+C at U-Boot prompt)
2. Check ZFS module in loader:
   ```
   > lsmod
   > load zfs
   > load geom_mirror  # if you have mirror pools
   > boot
   ```

3. Check pool status from single-user mode:
   ```bash
   zpool import -a  # Import all pools
   zpool status     # Check pool health
   ```

## Next Steps

1. **Rebuild kernel** with updated configuration
2. **Test boot** with new RPI5 kernel
3. **Verify ZFS** filesystem is recognized and mounted
4. **Stress test** with thermal sensor monitoring

## References

- FreeBSD Handbook: Kernel Configuration
- ARM64 NOTES: `/usr/src/sys/arm64/conf/NOTES`
- ZFS Documentation: Official FreeBSD ZFS guide
- Loader Configuration: `/boot/loader.conf` documentation
