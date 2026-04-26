#!/bin/sh
# rp1_eth_status.sh — snapshot status of the rp1_eth Milestone 1 module
#
# Usage: sh tools/rp1_eth_status.sh
# Requires: rp1_eth and bcm2712 kernel modules loaded

set -e

SYSCTL=/sbin/sysctl
KLDSTAT=/sbin/kldstat

section() { printf '\n=== %s ===\n' "$*"; }

# ---- Module load state --------------------------------------------------
section "Module status"
for mod in bcm2712 rp1_eth; do
    if $KLDSTAT -q -n "${mod}.ko" 2>/dev/null; then
        printf '  [OK] %s loaded\n' "$mod"
    else
        printf '  [--] %s NOT loaded\n' "$mod"
    fi
done

# ---- FDT metadata -------------------------------------------------------
section "FDT metadata (hw.rp1_eth.mac.*)"
for key in address phy_mode phy_mdio_addr; do
    node="hw.rp1_eth.mac.${key}"
    val=$($SYSCTL -n "$node" 2>/dev/null) || val='<unavailable>'
    printf '  %-30s = %s\n' "$node" "$val"
done

# ---- eth_cfg registers --------------------------------------------------
section "eth_cfg registers (hw.rp1_eth.cfg.*)"
for reg in control status clkgen clk2fc intr inte; do
    node="hw.rp1_eth.cfg.${reg}"
    val=$($SYSCTL -n "$node" 2>/dev/null) || val='<unavailable>'
    printf '  %-32s = %s\n' "$node" "$val"
done

# ---- Decoded STATUS -----------------------------------------------------
section "Decoded link state"
val=$($SYSCTL -n hw.rp1_eth.cfg.status_decoded 2>/dev/null) \
    || val='<unavailable>'
printf '  %s\n' "$val"

# ---- CLKGEN decode (RP-008370-DS-1 Table 139) ---------------------------
clkgen=$($SYSCTL -n hw.rp1_eth.cfg.clkgen 2>/dev/null) || clkgen=''
if [ -n "$clkgen" ]; then
    # bit 9 = TXCLKDELEN, bit 8 = DC50, bit 7 = ENABLE, bit 6 = KILL
    enable=$(( (clkgen >> 7) & 1 ))
    kill=$(( (clkgen >> 6) & 1 ))
    txdel=$(( (clkgen >> 9) & 1 ))
    printf '\n  CLKGEN: ENABLE=%d KILL=%d TXCLKDELEN=%d (raw 0x%08x)\n' \
        "$enable" "$kill" "$txdel" "$clkgen"
    if [ "$enable" -eq 0 ] || [ "$kill" -eq 1 ]; then
        printf '  WARNING: CLKGEN clock is STOPPED — STATUS will be frozen.\n'
        printf '           Module should have restarted it; try reloading.\n'
    fi
    if [ "$txdel" -ne 0 ]; then
        printf '  WARNING: CLKGEN.TXCLKDELEN=1 but phy-mode=rgmii-id\n'
        printf '           This will cause CRC errors at 1 Gbps.\n'
    fi
fi

printf '\n'
