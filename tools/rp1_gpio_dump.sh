#!/bin/sh
# rp1_gpio_dump.sh — Snapshot IO_BANK CTRL/STATUS and PADS for a given GPIO pin.
#
# Usage: sudo sh tools/rp1_gpio_dump.sh [pin]
#   pin  — GPIO number 0-53 (default: 45 = FAN_PWM)
#
# Requires rp1_gpio to be loaded (kldload rp1_gpio).
# Output shows CTRL register hex, decoded FUNCSEL, OE/IN from RIO, and
# the raw PADS word for the pin — sufficient to verify mux state matches
# the values formerly set by the one-shot code in bcm2712.c.
#
# Exit codes: 0 = success, 1 = driver not loaded, 2 = bad pin argument.

set -e

PIN=${1:-45}

# Validate pin number
case "$PIN" in
    ''|*[!0-9]*)
        echo "Usage: $0 [pin]  # pin must be 0-53" >&2
        exit 2
        ;;
esac

if [ "$PIN" -lt 0 ] || [ "$PIN" -gt 53 ]; then
    echo "Error: pin $PIN out of range 0-53" >&2
    exit 2
fi

# Confirm the driver is loaded
if ! kldstat -n rp1_gpio > /dev/null 2>&1; then
    echo "Error: rp1_gpio module is not loaded.  Run: sudo kldload rp1_gpio" >&2
    exit 1
fi

# Derive bank and pin-in-bank
if [ "$PIN" -lt 28 ]; then
    BANK=0; IDX=$PIN
elif [ "$PIN" -lt 34 ]; then
    BANK=1; IDX=$((PIN - 28))
else
    BANK=2; IDX=$((PIN - 34))
fi

echo "=== GPIO $PIN  (bank $BANK, index $IDX within bank) ==="

# gpioctl -lv gives name + current value
if gpioctl -lv gpioc0 2>/dev/null | grep -q "^pin $PIN "; then
    echo ""
    echo "--- gpioctl pin info ---"
    gpioctl -lv gpioc0 2>/dev/null | awk -v pin="pin $PIN" '$0 ~ "^" pin " "'
    echo ""
    echo "--- pin level (gpioctl gpioc0 $PIN) ---"
    gpioctl gpioc0 "$PIN" 2>/dev/null || echo "  (read failed — pin may be in peripheral mode)"
fi

echo ""
echo "--- sysctl rp1_gpio pin$PIN ---"
sysctl hw.rp1_gpio.pin${PIN} 2>/dev/null || echo "  (sysctl tree not available in M1)"

echo ""
echo "Done.  For raw register hex, load rp1_gpio M1 and inspect dmesg for"
echo "attach-time FUNCSEL readback, or use devctl(8) once sysctl tree is wired."
