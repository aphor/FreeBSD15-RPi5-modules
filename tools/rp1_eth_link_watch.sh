#!/bin/sh
# rp1_eth_link_watch.sh — poll eth_cfg.STATUS to observe cable plug/unplug
#
# Usage: sh tools/rp1_eth_link_watch.sh [interval_ms]
#   interval_ms  Poll interval in milliseconds (default: 500)
#
# Exit criteria for Milestone 1:
#   Plug/unplug the Ethernet cable and observe LINK toggling in the output.
#   SPEED and DUPLEX should update on plug events.
#
# Requires: rp1_eth kernel module loaded

INTERVAL_MS=${1:-500}
SYSCTL=/sbin/sysctl
NODE_STATUS="hw.rp1_eth.cfg.status_decoded"
NODE_RAW="hw.rp1_eth.cfg.status"

# Validate module is loaded
if ! /sbin/kldstat -q -n rp1_eth.ko 2>/dev/null; then
    echo "ERROR: rp1_eth module is not loaded." >&2
    echo "       Run: sudo kldload rp1_eth" >&2
    exit 1
fi

# Validate sysctl node exists
if ! $SYSCTL -n "$NODE_STATUS" >/dev/null 2>&1; then
    echo "ERROR: sysctl $NODE_STATUS not found." >&2
    echo "       Is the module fully initialised?" >&2
    exit 1
fi

echo "Watching $NODE_STATUS every ${INTERVAL_MS} ms  (Ctrl-C to stop)"
echo "Plug/unplug the Ethernet cable to observe link state changes."
echo "------------------------------------------------------------"

# Convert milliseconds to a sleep argument (sh arithmetic, no bc needed)
# FreeBSD sleep accepts fractional seconds
SLEEP_ARG=$(awk "BEGIN { printf \"%.3f\", $INTERVAL_MS / 1000.0 }")

prev=""
while true; do
    ts=$(date '+%H:%M:%S.%3N' 2>/dev/null || date '+%H:%M:%S')
    decoded=$($SYSCTL -n "$NODE_STATUS" 2>/dev/null)
    raw=$($SYSCTL -n "$NODE_RAW" 2>/dev/null)

    if [ "$decoded" != "$prev" ]; then
        printf '[%s] CHANGE  %s  (raw=0x%08x)\n' \
            "$ts" "$decoded" "$raw"
        prev="$decoded"
    else
        printf '[%s] %-60s\r' "$ts" "$decoded"
    fi
    sleep "$SLEEP_ARG"
done
