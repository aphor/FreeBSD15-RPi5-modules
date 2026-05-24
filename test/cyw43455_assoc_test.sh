#!/bin/sh
#
# test/cyw43455_assoc_test.sh — FullMAC WPA2 association test for cyw43455
# SPDX-License-Identifier: BSD-2-Clause
#
# Tests the cyw43455 firmware-side WPA2 (FullMAC) association path without
# wpa_supplicant, driving the state machine via ifconfig directly.
#
# Usage:
#   sudo sh test/cyw43455_assoc_test.sh [SSID [PSK [observe_seconds]]]
#
# Defaults match the localnet test AP used during development.
#
# Pass criteria:
#   E_LINK link=1 appears in dmesg AND no E_DISASSOC reason=8 follows within
#   the observation window.  Also checks rx_eio_count remains 0.
#
# Exit codes:
#   0  association established and held (link stayed up)
#   1  association failed or link dropped (reason=8 or no E_LINK at all)
#   2  setup error (module not loadable, interface not created, etc.)

SSID="${1:-localnet}"
PSK="${2:-VelcroDoggler}"
WAIT="${3:-15}"

WLANDEV="cyw434550"
WLANIF="wlan0"

# Source test utilities for log_* helpers if available from the test dir.
SCRIPT_DIR="$(dirname "$0")"
if [ -f "${SCRIPT_DIR}/test_utils.sh" ]; then
    . "${SCRIPT_DIR}/test_utils.sh"
else
    # Minimal fallbacks so the script runs standalone on dunn.
    log_info()    { echo "INFO: $*"; }
    log_pass()    { echo "PASS: $*"; }
    log_fail()    { echo "FAIL: $*"; }
    log_warning() { echo "WARN: $*"; }
fi

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

log_info "cyw43455 WPA2 association test — SSID='${SSID}' observe=${WAIT}s"

# Kill any running wpa_supplicant on the test interface so it cannot
# race against the firmware supplicant or intercept EAPOL frames.
if pgrep -q wpa_supplicant 2>/dev/null; then
    log_info "Stopping wpa_supplicant"
    killall wpa_supplicant 2>/dev/null
    sleep 1
fi

# Destroy any existing wlan0 vap.
if ifconfig "${WLANIF}" >/dev/null 2>&1; then
    log_info "Destroying existing ${WLANIF}"
    ifconfig "${WLANIF}" destroy 2>/dev/null
    sleep 1
fi

# Load the module if not already present.
if ! kldstat -n cyw43455 >/dev/null 2>&1; then
    log_info "Loading cyw43455 module"
    kldload cyw43455 || { log_fail "kldload cyw43455 failed"; exit 2; }
    sleep 2
fi

# Verify the physical device appeared.
if ! ifconfig -l | grep -qw "${WLANDEV}"; then
    log_fail "Physical device ${WLANDEV} not found after kldload"
    exit 2
fi

# Create the VAP.
log_info "Creating ${WLANIF} on ${WLANDEV}"
ifconfig "${WLANIF}" create wlandev "${WLANDEV}" || {
    log_fail "ifconfig create failed"
    exit 2
}

# Store the passphrase into the driver's write-only sysctl.
log_info "Setting PSK (${#PSK} chars)"
sysctl "hw.cyw43455.psk=${PSK}" >/dev/null || {
    log_fail "sysctl hw.cyw43455.psk set failed"
    exit 2
}

# Clear dmesg ring so the event grep below is scoped to this test run.
dmesg -c >/dev/null 2>&1

# ---------------------------------------------------------------------------
# Association
# ---------------------------------------------------------------------------

log_info "Bringing up ${WLANIF} with ssid='${SSID}' wpa"
ifconfig "${WLANIF}" ssid "${SSID}" wpa up || {
    log_fail "ifconfig ${WLANIF} up failed"
    exit 2
}

log_info "Observing for ${WAIT} seconds..."
sleep "${WAIT}"

# ---------------------------------------------------------------------------
# Collect evidence
# ---------------------------------------------------------------------------

echo ""
echo "=== ifconfig ${WLANIF} ==="
ifconfig "${WLANIF}" | grep -E '(status|ssid|bssid|authmode|privacy|inet)'

echo ""
echo "=== driver events (dmesg) ==="
dmesg | grep -E '(E_LINK|E_DISASSOC|E_AUTH|E_ASSOC|E_SET_SSID|set_pmk|security:)'

echo ""
echo "=== rx counters ==="
sysctl hw.cyw43455 | grep rx_

# ---------------------------------------------------------------------------
# Pass / fail evaluation
# ---------------------------------------------------------------------------

echo ""

# Did the link ever come up?
LINK_UP=0
dmesg | grep -q "E_LINK: link=1" && LINK_UP=1

# Did we get kicked with reason=8 (4-way handshake timeout) after link-up?
DISASSOC_8=0
dmesg | awk '
    /E_LINK: link=1/               { up=1 }
    /E_DISASSOC.*reason=8/ && up   { bad=1; exit }
    END                            { exit !bad }
' && DISASSOC_8=1

# Any F2 EIO?
EIO_COUNT=$(sysctl -n hw.cyw43455.rx_eio_count 2>/dev/null || echo 0)

RESULT=0

if [ "${LINK_UP}" -eq 0 ]; then
    log_fail "E_LINK link=1 never appeared — association did not complete"
    RESULT=1
else
    if [ "${DISASSOC_8}" -eq 1 ]; then
        log_fail "E_DISASSOC reason=8 after link=1 — 4-way handshake rejected by AP"
        RESULT=1
    else
        log_pass "Link came up and was NOT dropped by AP during observation window"
    fi
fi

if [ "${EIO_COUNT}" -gt 0 ]; then
    log_warning "rx_eio_count=${EIO_COUNT} — F2 EIO errors occurred (data path may be impaired)"
else
    log_pass "rx_eio_count=0 — no F2 EIO errors"
fi

if [ "${RESULT}" -eq 0 ]; then
    echo ""
    log_pass "cyw43455 WPA2 association test PASSED"
else
    echo ""
    log_fail "cyw43455 WPA2 association test FAILED"
fi

exit "${RESULT}"
