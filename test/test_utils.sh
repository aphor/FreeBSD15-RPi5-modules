#!/bin/sh
#
# Test Utilities for RPi5 Kernel Module Test Suite
# SPDX-License-Identifier: BSD-2-Clause

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# Test result tracking
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Logging
log_test_start() {
    TESTS_RUN=$((TESTS_RUN + 1))
    echo "${BLUE}[TEST $TESTS_RUN]${NC} $1"
}

log_pass() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    echo "${GREEN}✓ PASS:${NC} $1"
}

log_fail() {
    TESTS_FAILED=$((TESTS_FAILED + 1))
    echo "${RED}✗ FAIL:${NC} $1"
}

log_warning() {
    echo "${YELLOW}⚠ WARNING:${NC} $1"
}

log_info() {
    echo "${BLUE}ℹ INFO:${NC} $1"
}

# Module utilities
is_module_loaded() {
    kldstat | grep -q "$1"
}

wait_for_module() {
    local module="$1"
    local timeout="${2:-5}"
    local count=0

    while [ $count -lt $timeout ]; do
        if is_module_loaded "$module"; then
            return 0
        fi
        sleep 1
        count=$((count + 1))
    done
    return 1
}

# sysctl utilities
test_sysctl_readable() {
    sysctl "$1" >/dev/null 2>&1
}

test_sysctl_writable() {
    local sysctl_var="$1"
    local test_value="$2"
    local original_value

    # Get original value
    original_value=$(sysctl -n "$sysctl_var" 2>/dev/null) || return 1

    # Try to set test value
    sudo sysctl "${sysctl_var}=${test_value}" >/dev/null 2>&1 || return 1

    # Verify change
    local new_value
    new_value=$(sysctl -n "$sysctl_var" 2>/dev/null) || return 1

    # Restore original value
    sudo sysctl "${sysctl_var}=${original_value}" >/dev/null 2>&1

    [ "$new_value" = "$test_value" ]
}

# Test summary
print_test_summary() {
    echo ""
    echo "${BOLD}========================================${NC}"
    echo "${BOLD}Test Summary${NC}"
    echo "${BOLD}========================================${NC}"
    echo "Tests Run:    $TESTS_RUN"
    echo "Passed:       ${GREEN}$TESTS_PASSED${NC}"
    echo "Failed:       ${RED}$TESTS_FAILED${NC}"

    if [ $TESTS_FAILED -eq 0 ]; then
        echo ""
        echo "${GREEN}${BOLD}🎉 ALL TESTS PASSED!${NC}"
        return 0
    else
        echo ""
        echo "${RED}${BOLD}❌ SOME TESTS FAILED!${NC}"
        return 1
    fi
}