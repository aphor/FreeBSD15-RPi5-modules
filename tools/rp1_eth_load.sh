#!/bin/sh
# rp1_eth_load.sh — build, install, load rp1_eth and run status check
#
# Usage: sudo sh tools/rp1_eth_load.sh [--unload | --reload]
#   (no args)  build + install + load + status
#   --unload   unload rp1_eth only
#   --reload   unload + load (no rebuild)

set -e
REPO=$(dirname "$(realpath "$0")")/..
cd "$REPO"

case "${1:-}" in
--unload)
    if /sbin/kldstat -q -n rp1_eth.ko 2>/dev/null; then
        echo "Unloading rp1_eth..."
        /sbin/kldunload rp1_eth
        echo "Done."
    else
        echo "rp1_eth not loaded."
    fi
    exit 0
    ;;
--reload)
    if /sbin/kldstat -q -n rp1_eth.ko 2>/dev/null; then
        echo "Unloading rp1_eth..."
        /sbin/kldunload rp1_eth
    fi
    ;;
"")
    echo "=== Building rp1_eth ==="
    make rp1_eth
    echo "=== Installing ==="
    make install-rp1_eth
    ;;
*)
    echo "Usage: $0 [--unload | --reload]" >&2
    exit 1
    ;;
esac

echo "=== Loading rp1_eth ==="
/sbin/kldload rp1_eth

echo "=== Status ==="
sh tools/rp1_eth_status.sh
