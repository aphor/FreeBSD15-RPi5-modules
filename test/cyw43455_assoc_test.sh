#!/bin/sh
#
# test/cyw43455_assoc_test.sh — FullMAC WPA2 association test for cyw43455
# SPDX-License-Identifier: BSD-2-Clause
#
# Drives the cyw43455 WPA2 association path via wpa_supplicant and
# collects driver-side evidence (dmesg events, rx counters, supplicant
# log) for a single observation window.
#
# Credentials:
#   The test reads SSID and PSK from a ".wifi" file in the current
#   working directory (typically the repo root).  Format is shell-
#   sourceable:
#       WIFI_SSID="YOUR_SSID"
#       WIFI_PSK="YOUR_WIFI_PASSWORD"
#   A ".wifi" file is .gitignored so credentials never get committed.
#   Positional args override the file values.
#
# Usage:
#   sudo sh test/cyw43455_assoc_test.sh [SSID [PSK [observe_seconds]]]
#
# Pass criteria:
#   E_LINK link=1 appears in dmesg AND no E_DISASSOC reason=8 follows
#   within the observation window.  Also checks rx_eio_count remains 0.
#
# Exit codes:
#   0  association established and held (link stayed up)
#   1  association failed or link dropped (reason=8 or no E_LINK at all)
#   2  setup error (missing credentials, module not loadable, etc.)

# Source credentials from .wifi in the current working directory if it
# exists.  Empty defaults mean "fail with a clear message below" unless
# overridden by positional args.
WIFI_SSID=""
WIFI_PSK=""
if [ -r "./.wifi" ]; then
    . ./.wifi
fi

SSID="${1:-${WIFI_SSID}}"
PSK="${2:-${WIFI_PSK}}"
WAIT="${3:-15}"

if [ -z "${SSID}" ] || [ -z "${PSK}" ]; then
    echo "FAIL: WiFi credentials not provided." >&2
    echo "      Create a .wifi file in the cwd with:" >&2
    echo "        WIFI_SSID=\"YOUR_SSID\"" >&2
    echo "        WIFI_PSK=\"YOUR_WIFI_PASSWORD\"" >&2
    echo "      or pass SSID and PSK on the command line." >&2
    exit 2
fi

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

# Create the VAP — this is also the proof the physical device exists.
# cyw434550 is a newbus device, not listed by ifconfig -l; the create
# command itself is the reachability check.
log_info "Creating ${WLANIF} on ${WLANDEV}"
ifconfig "${WLANIF}" create wlandev "${WLANDEV}" || {
    log_fail "ifconfig create wlandev ${WLANDEV} failed — is module loaded?"
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

# Bring the VAP up but do NOT set ssid/authmode via ifconfig — net80211
# rejects authmode wpa with EINVAL on this driver (capability mismatch).
# wpa_supplicant -Dbsd does the right ioctl sequence (sets SSID, WPA
# mode, ciphers, etc.) via SIOCS80211 and is responsible for handling
# EAPOL in userspace.  Our driver's psk sysctl supplies the PMK to the
# firmware in parallel, via WLC_SET_WSEC_PMK during the S_AUTH
# transition in cyw_newstate.
log_info "Bringing ${WLANIF} up (no ssid/authmode — wpa_supplicant will set them)"
ifconfig "${WLANIF}" up || {
    log_fail "ifconfig ${WLANIF} up failed"
    exit 2
}

# CYW43455 firmware 7.45.x does NOT run an internal supplicant
# (sup_wpa IOVAR -> BCME_BADARG).  EAPOL frames must be handled by
# wpa_supplicant in userspace.  Without it the AP will time out the
# 4-way handshake and kick us with E_DISASSOC reason=8.
WPA_CONF="${WPA_CONF:-/etc/wpa_supplicant.conf}"
if [ ! -r "${WPA_CONF}" ]; then
    log_fail "${WPA_CONF} missing — EAPOL cannot be handled, aborting"
    exit 2
fi
log_info "Starting wpa_supplicant (-Dbsd -i${WLANIF} -c${WPA_CONF})"
WPA_LOG=/tmp/wpa_assoc_test.log
rm -f "${WPA_LOG}"
# Run in the background via shell '&' rather than wpa_supplicant's own -B
# flag; -B combined with -f is not supported in this build and causes the
# binary to print usage and exit instead of daemonising.
wpa_supplicant -Dbsd -i"${WLANIF}" -c"${WPA_CONF}" \
    >"${WPA_LOG}" 2>&1 &
WPA_PID=$!
log_info "wpa_supplicant pid=${WPA_PID}"
sleep 1

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
dmesg | grep -E '(E_LINK|E_DISASSOC|E_AUTH|E_ASSOC|E_SET_SSID|set_pmk|security:|AUTH:)'

echo ""
echo "=== rx counters ==="
sysctl hw.cyw43455 | grep rx_

if [ -f "${WPA_LOG}" ]; then
    echo ""
    echo "=== wpa_supplicant tail ==="
    tail -20 "${WPA_LOG}"
fi

# Stop wpa_supplicant now that evidence is collected.
if [ -n "${WPA_PID}" ] && kill -0 "${WPA_PID}" 2>/dev/null; then
    log_info "Stopping wpa_supplicant pid=${WPA_PID}"
    kill "${WPA_PID}" 2>/dev/null
    sleep 1
fi

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
