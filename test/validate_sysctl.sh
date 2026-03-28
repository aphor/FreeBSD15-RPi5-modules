#!/bin/sh
#
# sysctl Interface Validation Script
# Tests all RPi5 fan sysctl variables for proper functionality

. ./test_utils.sh

log_test_start "sysctl Interface Validation"

# Test sysctl tree existence
if test_sysctl_readable "hw.rpi5.fan"; then
    log_pass "hw.rpi5.fan tree exists"
else
    log_fail "hw.rpi5.fan tree not found"
    exit 1
fi

# Test temperature thresholds (readable and writable)
for i in 0 1 2 3; do
    sysctl_var="hw.rpi5.fan.temp$i"
    if test_sysctl_readable "$sysctl_var"; then
        log_pass "$sysctl_var is readable"

        # Test writability with valid value
        if test_sysctl_writable "$sysctl_var" "45000"; then
            log_pass "$sysctl_var is writable"
        else
            log_fail "$sysctl_var is not writable"
        fi
    else
        log_fail "$sysctl_var is not readable"
    fi
done

# Test hysteresis values (readable and writable)
for i in 0 1 2 3; do
    sysctl_var="hw.rpi5.fan.temp${i}_hyst"
    if test_sysctl_readable "$sysctl_var"; then
        log_pass "$sysctl_var is readable"

        if test_sysctl_writable "$sysctl_var" "7000"; then
            log_pass "$sysctl_var is writable"
        else
            log_fail "$sysctl_var is not writable"
        fi
    else
        log_fail "$sysctl_var is not readable"
    fi
done

# Test speed values (readable and writable)
for i in 0 1 2 3; do
    sysctl_var="hw.rpi5.fan.speed$i"
    if test_sysctl_readable "$sysctl_var"; then
        log_pass "$sysctl_var is readable"

        if test_sysctl_writable "$sysctl_var" "200"; then
            log_pass "$sysctl_var is writable"
        else
            log_fail "$sysctl_var is not writable"
        fi
    else
        log_fail "$sysctl_var is not readable"
    fi
done

# Test read-only values
readonly_vars="hw.rpi5.fan.current_state hw.rpi5.fan.cpu_temp"
for sysctl_var in $readonly_vars; do
    if test_sysctl_readable "$sysctl_var"; then
        log_pass "$sysctl_var is readable (read-only)"

        # Verify it's actually read-only
        if sudo sysctl "${sysctl_var}=999" >/dev/null 2>&1; then
            log_warning "$sysctl_var should be read-only but accepted write"
        else
            log_pass "$sysctl_var correctly rejects writes (read-only)"
        fi
    else
        log_fail "$sysctl_var is not readable"
    fi
done

# Test value validation (should reject invalid values)
log_info "Testing input validation..."

# Test invalid temperature (too high)
if ! sudo sysctl hw.rpi5.fan.temp0=150000 >/dev/null 2>&1; then
    log_pass "Temperature validation works (rejects 150°C)"
else
    log_warning "Temperature validation may be too permissive"
fi

# Test invalid speed (too high)
if ! sudo sysctl hw.rpi5.fan.speed0=300 >/dev/null 2>&1; then
    log_pass "Speed validation works (rejects 300)"
else
    log_warning "Speed validation may be too permissive"
fi

print_test_summary