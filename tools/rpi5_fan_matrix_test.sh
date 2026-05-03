#!/bin/sh
# rpi5_fan_matrix_test.sh — Fan state matrix test with CPU load
#
# Tests each fan state (0-4) independently under all-core CPU load.
# Each iteration resets thresholds to 90°C (fan off, CPU peaks briefly),
# reads a fresh cpu_temp T, then sets properly-ordered thresholds and a
# unique non-default speed for the active level.
#
# State 4 has no hysteresis in the kernel state machine — it drops the
# moment cpu < temp3 — so iteration 4 uses a short settle window to
# catch the state before the fan cools the CPU below the threshold.
#
# Usage: sudo sh tools/rpi5_fan_matrix_test.sh

SETTLE=5        # settle seconds for states 1-3
QUICKREAD=2     # fast read for state 4 (no hysteresis; cools quickly)
PIDS=""

snap() {
    local temp state rpm
    temp=$(sysctl -n hw.rpi5.fan.cpu_temp)
    state=$(sysctl -n hw.rpi5.fan.current_state)
    rpm=$(sysctl -n hw.rpi5.fan.rpm)
    printf "%-60s %6d mC  state=%d  rpm=%d\n" "$1" "$temp" "$state" "$rpm"
}

all_high() {
    sysctl -q hw.rpi5.fan.temp0=90000 hw.rpi5.fan.temp1=90000 \
               hw.rpi5.fan.temp2=90000 hw.rpi5.fan.temp3=90000
}

restore_defaults() {
    sysctl -q hw.rpi5.fan.temp0=50000  hw.rpi5.fan.temp1=60000  \
               hw.rpi5.fan.temp2=67500  hw.rpi5.fan.temp3=75000  \
               hw.rpi5.fan.speed0=75    hw.rpi5.fan.speed1=125   \
               hw.rpi5.fan.speed2=175   hw.rpi5.fan.speed3=250
}

cleanup() {
    kill $PIDS 2>/dev/null
    restore_defaults 2>/dev/null
}
trap cleanup EXIT INT TERM

# --- Start all-core CPU load ---
NCPU=$(sysctl -n hw.ncpu)
for i in $(seq 1 $NCPU); do
    yes > /dev/null &
    PIDS="$PIDS $!"
done
printf "Started %d-core load (PIDs:%s)\n" "$NCPU" "$PIDS"

# Warm up with defaults active so the fan controls temperature safely.
printf "Warming 15 s with default thresholds/speeds...\n"
restore_defaults
sleep 15

printf "\n=== Fan State Matrix Test  (%d-core load) ===\n" "$NCPU"
printf "Unique non-default speeds: state1=60  state2=110  state3=165  state4=215\n"
printf "Active threshold: T-5000 mC below live cpu_temp; lower floors at T-8/11/14k\n\n"
printf "%-60s  %8s  %5s  %6s\n" "Iteration" "cpu_temp" "state" "rpm"
printf -- "%.0s-" $(seq 1 82); printf "\n"

# --- Iteration 0: baseline with all thresholds above any reachable temp ---
all_high
sleep $SETTLE
snap "0  thresholds=90°C  (fan off, load baseline)"

# peak_T: push thresholds high, wait 2 s for CPU to rise, return temperature.
# Redirects all_high output to /dev/null so $() capture picks up only the
# temperature integer from the final sysctl read.
peak_T() {
    all_high >/dev/null 2>&1
    sleep 2
    sysctl -n hw.rpi5.fan.cpu_temp
}

# --- Iteration 1: target state 1 ---
# Active:   temp0 = T-5000, speed0 = 60
# Inactive: temp1/2/3 remain at 90000
T=$(peak_T)
T0=$((T - 5000))
sysctl -q hw.rpi5.fan.temp0=$T0 hw.rpi5.fan.speed0=60
sleep $SETTLE
snap "1  temp0=$T0 (T-5°C)   speed0=60   → state 1?"

# --- Iteration 2: target state 2 ---
# Active:  temp1 = T-5000, speed1 = 110
# Floor:   temp0 = T-8000, speed0 = 60
T=$(peak_T)
T0=$((T - 8000)); T1=$((T - 5000))
sysctl -q hw.rpi5.fan.temp0=$T0 hw.rpi5.fan.speed0=60  \
           hw.rpi5.fan.temp1=$T1 hw.rpi5.fan.speed1=110
sleep $SETTLE
snap "2  temp1=$T1 (T-5°C)   speed1=110  → state 2?"

# --- Iteration 3: target state 3 ---
# Active:  temp2 = T-5000, speed2 = 165
# Floors:  temp0 = T-11000, temp1 = T-8000
T=$(peak_T)
T0=$((T - 11000)); T1=$((T - 8000)); T2=$((T - 5000))
sysctl -q hw.rpi5.fan.temp0=$T0 hw.rpi5.fan.speed0=60  \
           hw.rpi5.fan.temp1=$T1 hw.rpi5.fan.speed1=110 \
           hw.rpi5.fan.temp2=$T2 hw.rpi5.fan.speed2=165
sleep $SETTLE
snap "3  temp2=$T2 (T-5°C)   speed2=165  → state 3?"

# --- Iteration 4: target state 4 ---
# Active:  temp3 = T-5000, speed3 = 215
# State 4 has no hysteresis: the kernel exits it the moment cpu < temp3.
# Use a 2 s quick-read to catch state=4 before the fan cools the CPU
# below the threshold, then a full-settle read to show what stabilises.
T=$(peak_T)
T0=$((T - 14000)); T1=$((T - 11000)); T2=$((T - 8000)); T3=$((T - 5000))
sysctl -q hw.rpi5.fan.temp0=$T0 hw.rpi5.fan.speed0=60  \
           hw.rpi5.fan.temp1=$T1 hw.rpi5.fan.speed1=110 \
           hw.rpi5.fan.temp2=$T2 hw.rpi5.fan.speed2=165 \
           hw.rpi5.fan.temp3=$T3 hw.rpi5.fan.speed3=215
sleep $QUICKREAD
snap "4a temp3=$T3 (T-5°C)   speed3=215  → state 4? (${QUICKREAD}s quick-read)"
sleep $((SETTLE - QUICKREAD))
snap "4b  ...full ${SETTLE}s settle  (no hysteresis: may have cooled below temp3)"

# --- Teardown ---
printf "\n=== Stop load + restore defaults ===\n"
kill $PIDS 2>/dev/null
PIDS=""
restore_defaults
sleep 6
snap "5  load off, defaults restored"
