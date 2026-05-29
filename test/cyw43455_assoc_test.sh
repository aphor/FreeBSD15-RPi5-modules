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
# Pass criteria (all must hold; each is evaluated against the
# observation window only, using the [<unix_epoch>] driver event
# timestamps so leftover dmesg / teardown events don't pollute):
#   1. E_LINK link=1 fires (firmware-side join completed)
#   2. No E_DISASSOC reason=8 inside the observation window
#   3. A non-link-local SLAAC autoconf address appears on wlan0
#      (proves RA decryption with the installed GTK, and DAD frame
#      TX with the installed PTK)
#   4. ping6 to the IPv6 router learned via ndp -rn round-trips
#      over wlan0 (proves the data path works in both directions)
# rx_eio_count remaining 0 is reported but does not gate exit code.
#
# Exit codes:
#   0  all four criteria above met
#   1  association failed, link dropped, SLAAC failed, or ping6 failed
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
# Also record a wall-clock start timestamp; the driver tags each event
# with [<unix_epoch>] (see cyw43455_events.c / cyw43455_security.c) so
# the pass/fail awk below can ignore any leftover dmesg from earlier
# runs AND any teardown events emitted after wpa_supplicant is killed.
dmesg -c >/dev/null 2>&1
START_EPOCH=$(date +%s)

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

# Enable IPv6 SLAAC on wlan0: clear IFDISABLED (FreeBSD sets it on new
# wlan interfaces by default), accept router advertisements, and let
# the kernel form an auto-linklocal address.  This is what gives us:
#   - Periodic outbound NDP (router solicitations + neighbour
#     solicitations to verify the router LL stays reachable) which
#     keeps the AP from disassoc'ing us for inactivity.
#   - A SLAAC global address as proof the data path is intact
#     end-to-end (RA arrival proves GTK install; the kernel responding
#     with an NS for DAD on the candidate address proves PTK TX).
# Per-interface accept_rtadv — not global sysctl — so this test does
# not affect rp1eth0's existing IPv6 default route which carries SSH.
log_info "Enabling IPv6 SLAAC on ${WLANIF}"
ifconfig "${WLANIF}" inet6 -ifdisabled accept_rtadv auto_linklocal 2>/dev/null \
    || log_warning "ifconfig ${WLANIF} inet6 setup failed (continuing)"

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
# IPv6 SLAAC + ping6 connectivity probe
# ---------------------------------------------------------------------------
# By the time the observation window ends the AP has had ample time
# to emit a router advertisement (typical period 30–600 s).  If our
# data path is healthy we should see an "autoconf" inet6 address on
# wlan0 — its presence proves:
#   - RA arrived (broadcast → AES-CCM decrypted with the installed GTK)
#   - DAD completed (we successfully transmitted neighbour solicitation
#     frames via the installed PTK)
# After confirming SLAAC, ping6 the IPv6 router (link-local from
# ndp -rn) to round-trip a unicast frame through the encrypted link.
SLAAC_OK=0
PING6_OK=0
SLAAC_ADDR=""
GW6=""

# Allow up to ~10 s for an RA-induced autoconf address to land and
# leave the tentative (DAD-in-progress) state.
for _i in 1 2 3 4 5 6 7 8 9 10; do
    SLAAC_ADDR=$(ifconfig "${WLANIF}" inet6 2>/dev/null | awk '
        /^[[:space:]]*inet6 / && /autoconf/ && !/tentative/ && !/deprecated/ {
            # Strip %scope suffix if present
            sub(/%.*/, "", $2)
            # Skip link-local — fe80::/10
            if ($2 !~ /^fe80:/) { print $2; exit }
        }')
    if [ -n "${SLAAC_ADDR}" ]; then
        SLAAC_OK=1
        break
    fi
    sleep 1
done

if [ "${SLAAC_OK}" -eq 1 ]; then
    log_info "SLAAC global address: ${SLAAC_ADDR}"
    # Find the IPv6 router on wlan0 (link-local).  ndp -rn output:
    #   <addr>   if=<if> [flags...]
    GW6=$(ndp -rn 2>/dev/null | awk -v IF="${WLANIF}" '
        $0 ~ ("if=" IF) { print $1; exit }
    ')
    if [ -n "${GW6}" ]; then
        log_info "ping6 -c 3 -t 5 -I ${WLANIF} ${GW6}"
        if ping6 -c 3 -t 5 -I "${WLANIF}" "${GW6}" >/tmp/ping6.log 2>&1; then
            PING6_OK=1
        fi
    else
        log_warning "no IPv6 router learned on ${WLANIF} — skipping ping6"
    fi
else
    log_warning "no SLAAC global address acquired on ${WLANIF}"
fi

# Mark the end of the observation window BEFORE collecting evidence /
# killing wpa_supplicant — events emitted during teardown should not
# count against the pass/fail evaluation (the AP-initiated disassoc
# from us killing wpa_supplicant is expected behaviour, not a failure).
END_EPOCH=$(date +%s)

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

echo ""
echo "=== IPv6 SLAAC + ping6 ==="
echo "SLAAC_ADDR=${SLAAC_ADDR:-<none>}  GW6=${GW6:-<none>}"
if [ -f /tmp/ping6.log ]; then
    grep -E 'PING|bytes from|packets transmitted' /tmp/ping6.log || cat /tmp/ping6.log
fi

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

# Scope pass/fail evaluation to events whose [<unix_epoch>] timestamp
# falls inside the observation window [START_EPOCH, END_EPOCH].  The
# driver tags every wifi-connection event line with that prefix (see
# cyw43455_events.c cyw_event_dispatch and the per-event handlers in
# cyw43455_security.c).  Lines without a timestamp prefix are ignored
# — they cannot be attributed to a specific run and shouldn't drive
# pass/fail.  This replaces the previous unscoped `dmesg | grep`, which
# saw E_DISASSOC events from earlier runs AND from teardown after the
# wpa_supplicant kill.
EVAL_OUTPUT=$(dmesg | awk -v start="${START_EPOCH}" -v end="${END_EPOCH}" '
    # Extract the leading [<epoch>] timestamp.  Skip lines without one.
    match($0, /\[[0-9]+\]/) {
        ts = substr($0, RSTART + 1, RLENGTH - 2) + 0
        if (ts < start || ts > end) next
        if (/E_LINK: link=1/)            up = 1
        if (/E_DISASSOC.*reason=8/ && up) bad = 1
    }
    END { printf "%d %d\n", up, bad }
')
LINK_UP=$(echo "${EVAL_OUTPUT}" | awk '{print $1+0}')
DISASSOC_8=$(echo "${EVAL_OUTPUT}" | awk '{print $2+0}')

# Any F2 EIO?
EIO_COUNT=$(sysctl -n hw.cyw43455.rx_eio_count 2>/dev/null || echo 0)

RESULT=0

if [ "${LINK_UP}" -eq 0 ]; then
    log_fail "E_LINK link=1 never appeared — association did not complete"
    RESULT=1
else
    if [ "${DISASSOC_8}" -eq 1 ]; then
        log_fail "E_DISASSOC reason=8 after link=1 — link dropped during observation"
        RESULT=1
    else
        log_pass "Link came up and was NOT dropped by AP during observation window"
    fi
fi

# IPv6 SLAAC + ping6: positive evidence the data path is working at
# Layer 3.  Treated as required because the whole point of bringing
# up the link is end-to-end connectivity, and the periodic NDP these
# checks induce is what keeps the AP from disassoc'ing for idle.
if [ "${SLAAC_OK}" -eq 1 ]; then
    log_pass "IPv6 SLAAC succeeded — wlan0 has global ${SLAAC_ADDR}"
else
    log_fail "IPv6 SLAAC did not yield a global address on ${WLANIF}"
    RESULT=1
fi
if [ "${PING6_OK}" -eq 1 ]; then
    log_pass "ping6 to ${GW6} via ${WLANIF} succeeded"
else
    log_warning "ping6 did not get a reply (may indicate AP rejecting STA traffic)"
    RESULT=1
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
