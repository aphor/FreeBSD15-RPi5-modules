#!/bin/sh
#
# Module Dependency Testing Script
# Tests the bcm2712 <-> rpi5 dependency relationship

. ./test_utils.sh

log_test_start "Module Dependency Testing"

# Clean slate
log_info "Cleaning up any existing modules..."
sudo kldunload rpi5 >/dev/null 2>&1 || true
sudo kldunload bcm2712 >/dev/null 2>&1 || true

if is_module_loaded "bcm2712" || is_module_loaded "rpi5"; then
    log_fail "Failed to unload existing modules"
    exit 1
fi

# Test 1: Load bcm2712 independently
log_info "Test 1: Loading bcm2712 independently..."
if sudo kldload bcm2712 >/dev/null 2>&1; then
    if wait_for_module "bcm2712" 3; then
        log_pass "bcm2712 loads independently"
    else
        log_fail "bcm2712 failed to load properly"
        exit 1
    fi
else
    log_fail "Failed to load bcm2712"
    exit 1
fi

# Clean up for next test
sudo kldunload bcm2712 >/dev/null 2>&1

# Test 2: Load rpi5 (should auto-load bcm2712)
log_info "Test 2: Loading rpi5 (should auto-load bcm2712)..."
if sudo kldload rpi5 >/dev/null 2>&1; then
    # Check both modules are loaded
    if wait_for_module "rpi5" 3 && wait_for_module "bcm2712" 3; then
        log_pass "rpi5 auto-loaded bcm2712 dependency"
    else
        log_fail "Dependency auto-loading failed"
        exit 1
    fi
else
    log_fail "Failed to load rpi5"
    exit 1
fi

# Test 3: Try to unload bcm2712 while rpi5 depends on it
log_info "Test 3: Testing dependency protection..."
if sudo kldunload bcm2712 2>&1 | grep -q "Device busy"; then
    log_pass "bcm2712 correctly protected from unloading (dependency active)"
else
    log_warning "bcm2712 dependency protection may not be working"
fi

# Both modules should still be loaded
if is_module_loaded "bcm2712" && is_module_loaded "rpi5"; then
    log_pass "Both modules remain loaded after failed bcm2712 unload"
else
    log_fail "Module state inconsistent after dependency test"
    exit 1
fi

# Test 4: Proper unload sequence (rpi5 first, then bcm2712)
log_info "Test 4: Testing proper unload sequence..."

# Unload rpi5 first
if sudo kldunload rpi5 >/dev/null 2>&1; then
    log_pass "rpi5 unloaded successfully"
else
    log_fail "Failed to unload rpi5"
    exit 1
fi

# bcm2712 might auto-unload or remain loaded
sleep 2

if ! is_module_loaded "rpi5"; then
    log_pass "rpi5 confirmed unloaded"
else
    log_fail "rpi5 still loaded after unload command"
fi

# Try to unload bcm2712 now (should work)
if is_module_loaded "bcm2712"; then
    if sudo kldunload bcm2712 >/dev/null 2>&1; then
        log_pass "bcm2712 unloaded after rpi5 dependency removed"
    else
        log_warning "bcm2712 did not unload cleanly"
    fi
fi

# Verify clean state
if ! is_module_loaded "bcm2712" && ! is_module_loaded "rpi5"; then
    log_pass "Clean unload completed - no modules loaded"
else
    log_fail "Modules still loaded after unload sequence"
    exit 1
fi

print_test_summary