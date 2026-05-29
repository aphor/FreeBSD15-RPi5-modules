# Rebuilding RPI5 Kernel with ZFS Root Filesystem Support

## Overview

The RPI5 kernel configuration has been updated to include ZFS filesystem support. This document provides step-by-step instructions to rebuild the kernel and test the ZFS root filesystem mounting.

## Prerequisites

- FreeBSD 16-CURRENT ARM64 with `/usr/src/` available
- Backup of current kernel in `/boot/kernel.old` (optional but recommended)
- ZFS root filesystem `zfs:dunn/ROOT/default` ready to mount
- Loader configuration with `zfs_load="YES"` in `/boot/loader.conf`

## Changes Made to RPI5 Configuration

**File**: `/usr/src/sys/arm64/conf/RPI5`

```
options 	ZFS
```

This single line addition:
- Compiles ZFS filesystem driver into kernel
- Makes ZFS filesystem type available at mountroot time
- Enables proper root filesystem detection for `zfs:dunn/ROOT/default`

## Step 1: Clean Old Build Artifacts (Optional but Recommended)

```bash
cd /usr/src
sudo make KERNCONF=RPI5 clean
```

Output: Removes old kernel objects and build artifacts.

## Step 2: Build New Kernel

```bash
cd /usr/src
sudo make -j$(sysctl -n hw.ncpu) KERNCONF=RPI5 buildkernel
```

**Expected Duration**: 30-90 minutes depending on RPi5 hardware

**What to watch for**:
- No compilation errors in `bcm2712_thermal.c`
- No link errors when creating final kernel
- Final message should be: `"Kernel build for RPI5 completed"`

**Typical Progress**:
```
>>> Kernel build for RPI5 started on ...
>>> stage 1: configuring the kernel
>>> stage 2: building system ...
>>> stage 3: making dependencies ...
>>> stage 4: building everything ...
>>> Kernel build for RPI5 completed on ...
```

## Step 3: Install New Kernel

```bash
cd /usr/src
sudo make KERNCONF=RPI5 installkernel
```

This will:
- Copy new kernel to `/boot/kernel/`
- Preserve old kernel in `/boot/kernel.old/`
- Update loader configuration references

**Verify installation**:
```bash
ls -lh /boot/kernel/kernel
ls -lh /boot/kernel.old/kernel  # Should exist
```

## Step 4: Reboot with New Kernel

```bash
sudo shutdown -r now
```

Or more graceful:
```bash
sudo shutdown -r +5 "Rebooting to test ZFS-enabled RPI5 kernel"
```

**During Boot**:
You should see:
```
Trying to mount root from zfs:dunn/ROOT/default []...
```

Followed by successful mount and boot completion.

## Step 5: Verify ZFS Support in New Kernel

After boot, verify ZFS is compiled in:

```bash
# Check ZFS filesystem is available
sysctl kern.features.zfs
# Expected output: kern.features.zfs: 1

# Check mounted root filesystem
mount | grep " on / "
# Expected output: zfs:dunn/ROOT/default on / (zfs, local, journaled, log)

# Verify ZFS pool
zpool status
# Should show pool healthy and mounted

# Verify kernel version
uname -a
# Should show "RPI5" in kernel name

# Check kernel config included ZFS
grep -i zfs /boot/kernel/config.txt 2>/dev/null | head -5
```

## Step 6: Verify BCM2712 Thermal Sensor

The thermal sensor driver should also be loaded:

```bash
# Check thermal driver is recognized
dmesg | grep bcm2712_thermal

# Check thermal sensor sysctl
sysctl hw.bcm2712.thermal.cpu_temp
# Expected: hw.bcm2712.thermal.cpu_temp: XX.XC (temperature in Celsius)

# Monitor temperature
watch -n 1 'sysctl hw.bcm2712.thermal.cpu_temp'
```

## Fallback: If Boot Fails

If the new kernel fails to boot:

### 1. Boot to U-Boot/Barebox Prompt

Interrupt boot sequence:
- Wait for bootloader prompt
- Press Ctrl+C or appropriate key for your bootloader

### 2. Load Alternative Kernel

```
> boot /boot/kernel.old/kernel
```

Or load specific modules before boot:
```
> load zfs
> load geom_mirror
> boot
```

### 3. Boot to Single-User Mode (if kernel loads but fails)

From GRUB or U-Boot:
- Add `-s` flag to boot command for single-user mode
- Start SSH or Serial for remote access
- Check `/var/log/messages` for errors

### 4. Investigate Issues

```bash
# Check ZFS module
ls -lh /boot/modules/zfs.ko

# Test ZFS module loading
sudo modload -p zfs

# Check kernel configuration
file /boot/kernel/kernel
strings /boot/kernel/kernel | grep -i "zfs\|options" | head -10

# Check system logs
tail -50 /var/log/messages | grep -i "zfs\|kernel\|error"
```

## Build Configuration Reference

### Current RPI5 Kernel Config

**Location**: `/usr/src/sys/arm64/conf/RPI5`

```
cpu		ARM64
ident		RPI5

include		"GENERIC"
makeoptions	DEBUG=-g
options 	ZFS
```

### Files Contributing to Configuration

1. **Main config**: `RPI5` - Custom ARM64 config for Raspberry Pi 5
2. **Includes GENERIC**: Provides base system devices and options
3. **Includes std.broadcom**: Adds Broadcom SoC support
4. **Includes std.dev**: Standard device drivers
5. **Includes std.arm64**: ARM64 standard options

## Cleanup (If Everything Works)

After successful boot and testing, you can optionally clean up old kernel:

```bash
# Backup old kernel (optional)
sudo cp /boot/kernel.old/kernel /boot/kernel.old.backup

# Remove old kernel to save space (optional, keep for 1-2 weeks as safety net)
sudo rm -rf /boot/kernel.old

# Clean build artifacts
cd /usr/src
sudo make KERNCONF=RPI5 clean
```

## Testing Checklist

- [ ] New kernel builds without errors
- [ ] Installation succeeds with `make installkernel`
- [ ] System boots with ZFS root filesystem
- [ ] `mount` shows ZFS root mounted
- [ ] `zpool status` shows pool healthy
- [ ] `kern.features.zfs` sysctl returns 1
- [ ] BCM2712 thermal sensor sysctl responds
- [ ] System fully operational under load

## Performance Notes

- **Build time**: 45-90 minutes on RPi5 (4-core ARM64 CPU)
- **Boot time**: Should be similar to previous kernel (~10-20 seconds)
- **Filesystem**: ZFS with on-disk SSD/NVMe should perform well
- **Memory**: ZFS ARC cache will use available RAM (~25-50% typical)

## Reverting Changes (If Needed)

To revert to original configuration:

```bash
# Edit RPI5 config
sudo vi /usr/src/sys/arm64/conf/RPI5

# Remove the line:
# options 	ZFS

# Rebuild and reinstall
cd /usr/src
sudo make KERNCONF=RPI5 clean
sudo make -j$(sysctl -n hw.ncpu) KERNCONF=RPI5 buildkernel
sudo make KERNCONF=RPI5 installkernel
sudo shutdown -r now
```

## Support Resources

- **FreeBSD Handbook**: https://docs.freebsd.org/en/books/handbook/
- **ZFS Documentation**: https://wiki.freebsd.org/ZFS
- **Kernel Configuration**: https://docs.freebsd.org/en/books/handbook/kernelconfig/
- **FreeBSD Forum**: https://forums.freebsd.org/

## Summary

The ZFS support has been added to the RPI5 kernel configuration through a single option line:
```
options 	ZFS
```

This ensures ZFS filesystem support is available at boot time for proper root filesystem mounting. Follow the steps above to rebuild, install, and test the new kernel.

**Expected Result**: System boots successfully with ZFS root filesystem mounted and all services operational.
