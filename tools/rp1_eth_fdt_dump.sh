#!/bin/sh
# rp1_eth_fdt_dump.sh — dump FDT nodes relevant to the RP1 Ethernet driver
#
# Usage: sh tools/rp1_eth_fdt_dump.sh
# Requires: ofwdump(8), root privileges (reads /dev/openfirm)
#
# Dumps:
#   1. Root node compatible (Pi 5 variant check)
#   2. RP1 pcie@120000 node (address map + interrupt routing)
#   3. ethernet@100000 node (MAC, phy-mode, phy-reset-gpios)
#   4. ethernet-phy@1 child (MDIO addr, Broadcom-specific properties)

OFWDUMP=/usr/sbin/ofwdump

# Check permissions
if [ "$(id -u)" -ne 0 ]; then
    echo "NOTE: ofwdump may need root to read /dev/openfirm." >&2
    echo "      Re-run with: sudo sh $0" >&2
    echo ""
fi

if ! command -v "$OFWDUMP" >/dev/null 2>&1; then
    echo "ERROR: ofwdump not found at $OFWDUMP" >&2
    exit 1
fi

section() { printf '\n=== %s ===\n' "$*"; }

# ---- Root node ----------------------------------------------------------
section "Root node (Pi 5 variant check)"
$OFWDUMP -p / 2>/dev/null | grep -E '(compatible|model|serial)' | head -10

# ---- PCIe node (RP1 host bridge) ----------------------------------------
section "pcie@120000 (BCM2712 pcie2 — RP1 host bridge)"
$OFWDUMP -p -a /axi/pcie@120000 2>/dev/null \
    | grep -E '(compatible|reg|ranges|interrupts|#address)' \
    | head -20

# ---- RP1 Ethernet node --------------------------------------------------
section "ethernet@100000 (Cadence GEM_GXL — raspberrypi,rp1-gem)"
$OFWDUMP -p -a /axi/pcie@120000/rp1/ethernet@100000 2>/dev/null

# ---- PHY child node ------------------------------------------------------
section "ethernet-phy@1 (BCM PHY child)"
$OFWDUMP -p -a /axi/pcie@120000/rp1/ethernet@100000/ethernet-phy@1 2>/dev/null

# ---- GPIO phandle (phy-reset-gpios) -------------------------------------
section "GPIO controller (for phy-reset-gpios phandle)"
$OFWDUMP -p -a /axi/pcie@120000/rp1/gpio@d0000 2>/dev/null \
    | grep -E '(compatible|reg|phandle|#gpio)' | head -10

printf '\n'
