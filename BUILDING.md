# Building and Installing the RPi5 Cooling Fan Sysctl Module

## System Requirements

- **OS**: FreeBSD 15.0 or later
- **Hardware**: Raspberry Pi 5
- **Kernel Sources**: Required for module compilation
- **Build Tools**: gcc/clang, make

## Checking Your FreeBSD Version

```bash
freebsd-version
uname -a
```

## Prerequisites Installation

### Install Kernel Sources

If you don't have kernel sources installed:

```bash
# Using freebsd-update (easiest for pre-built systems)
sudo freebsd-update fetch
sudo freebsd-update install

# Or using ports (if you prefer)
cd /usr/ports/sys/freebsd-src
sudo make install clean
```

Verify kernel sources are in place:
```bash
ls -la /usr/src/sys
```

## Building the Module

### Method 1: Standard Build and Install

```bash
# Create a working directory
mkdir -p ~/src/rpi5_cooling_fan
cd ~/src/rpi5_cooling_fan

# Copy the module files
cp /path/to/rpi5_cooling_fan.c .
cp /path/to/Makefile .

# Build the module
make

# Install the module (requires root)
sudo make install

# Verify installation
ls -la /boot/kernel/rpi5_cooling_fan.ko
```

### Method 2: Build and Load Without Installation

```bash
# Build only
make

# Load directly from build directory (for testing)
sudo kldload ./rpi5_cooling_fan.ko

# Test the sysctl interface
sysctl hw.rpi5.cooling_fan

# Unload for cleanup
sudo kldunload rpi5_cooling_fan
```

## Module Loading

### Manual Loading

```bash
# Load the module
sudo kldload rpi5_cooling_fan

# Verify it's loaded
kldstat | grep rpi5_cooling_fan

# Check sysctl interface
sysctl -a hw.rpi5.cooling_fan
```

### Automatic Loading at Boot

#### Option A: Using rc.conf

Edit `/etc/rc.conf`:
```sh
# Add this line
kld_list="rpi5_cooling_fan"
```

#### Option B: Using loader.conf

Edit `/boot/loader.conf`:
```sh
# Add this line
rpi5_cooling_fan_load="YES"
```

#### Option C: Using /etc/modules

Create or edit `/etc/modules`:
```
rpi5_cooling_fan
```

### Verify Auto-Loading After Boot

```bash
# Check module status
kldstat | grep rpi5_cooling_fan

# Should see output showing the module is loaded
# Example:
# 2    1 0xffffffff84f19000  1de0   rpi5_cooling_fan.ko
```

## Testing the Module

### Basic Functionality Test

```bash
# View all OIDs
sysctl -a hw.rpi5.cooling_fan

# Read a specific value
sysctl hw.rpi5.cooling_fan.temp0

# Modify a value (example: change temp0 to 45°C)
sudo sysctl hw.rpi5.cooling_fan.temp0=45000

# Verify the change
sysctl hw.rpi5.cooling_fan.temp0
```

### Using the Control Script

```bash
# Make the script executable
chmod +x rpi5_fan_control.sh

# Show all settings
./rpi5_fan_control.sh --show

# Set aggressive cooling
sudo ./rpi5_fan_control.sh --aggressive

# Reset to defaults
sudo ./rpi5_fan_control.sh --reset

# Monitor fan state
sudo ./rpi5_fan_control.sh --monitor
```

## Making Changes Persistent

### Using sysctl.conf

Edit `/etc/sysctl.conf` and add your settings:

```sh
# RPi5 Cooling Fan Configuration
hw.rpi5.cooling_fan.temp0=45000
hw.rpi5.cooling_fan.temp1=55000
hw.rpi5.cooling_fan.temp2=65000
hw.rpi5.cooling_fan.temp3=75000
hw.rpi5.cooling_fan.speed0=100
hw.rpi5.cooling_fan.speed1=150
hw.rpi5.cooling_fan.speed2=200
hw.rpi5.cooling_fan.speed3=255
```

Then reload:
```bash
sudo service sysctl restart
```

### Verification

```bash
# Check that settings are applied
sysctl hw.rpi5.cooling_fan
```

## Troubleshooting

### Module Won't Load

**Issue**: "kldload: can't load rpi5_cooling_fan.ko: No such file or directory"

**Solution**:
1. Verify the .ko file exists: `ls -la rpi5_cooling_fan.ko`
2. Use absolute path: `kldload /boot/kernel/rpi5_cooling_fan.ko`
3. Rebuild if needed: `make clean && make`

### Compilation Errors

**Issue**: "error: undefined reference to..."

**Solution**:
1. Verify kernel sources are installed
2. Clean rebuild: `make clean && make`
3. Check Makefile is in the correct directory
4. Ensure FreeBSD version matches kernel sources

### Sysctl OIDs Not Available

**Issue**: "sysctl: unknown oid 'hw.rpi5.cooling_fan'"

**Solution**:
1. Verify module is loaded: `kldstat | grep rpi5`
2. Check system messages: `dmesg | tail -20`
3. Reload module: `sudo kldunload rpi5_cooling_fan && sudo kldload rpi5_cooling_fan`

## Performance and Debugging

### Enable Debug Output

Build with debug symbols:
```bash
make DEBUG_FLAGS="-g"
```

### Monitor Kernel Messages

```bash
# Watch kernel messages in real-time
tail -f /var/log/messages

# Or use dmesg
dmesg | tail -20
```

### Check Module Information

```bash
# Show module details
kldstat -v | grep -A 20 rpi5_cooling_fan

# Show all loaded modules
kldstat
```

## Cleaning Up

### Remove Module Binary

```bash
sudo rm /boot/kernel/rpi5_cooling_fan.ko
```

### Unload from Running System

```bash
sudo kldunload rpi5_cooling_fan
```

### Clean Build Artifacts

```bash
cd ~/src/rpi5_cooling_fan
make clean
```

## Advanced: Customizing the Module

### Source Code Modifications

Edit `rpi5_cooling_fan.c` to modify:
- Default temperature thresholds
- Default PWM speeds
- Hysteresis values
- Validation ranges

### Recompilation After Changes

```bash
# Clean previous build
make clean

# Rebuild
make

# Reinstall
sudo make install

# Reload module
sudo kldunload rpi5_cooling_fan  # if loaded
sudo kldload rpi5_cooling_fan
```

## Integration with Other Tools

### Integration with thermal-agent (if available)

Create a wrapper script that reads sysctl values and makes decisions based on actual hardware temperature.

### Integration with Monitoring Tools

Example with `systat`:
```bash
# Monitor in syscall view (shows system activity)
systat -syscall

# Then in another terminal
watch 'sysctl hw.rpi5.cooling_fan'
```

## References

- FreeBSD Handbook: https://docs.freebsd.org/
- FreeBSD Kernel Module Programming: https://docs.freebsd.org/doc/en_US.ISO8859-1/books/arch/index.html
- sysctl(3) man page: `man 3 sysctl`
- sysctl(8) man page: `man 8 sysctl`
- Device tree documentation for Raspberry Pi 5

## Support

For issues or questions:
1. Check the README.md for usage examples
2. Review FreeBSD documentation
3. Examine kernel messages: `dmesg`
4. Verify module is compatible with your FreeBSD version
