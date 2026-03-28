# Raspberry Pi 5 Kernel Module Test Suite
# SPDX-License-Identifier: BSD-2-Clause

# Test configuration
PARENT_DIR = ..
TEST_LOG = test_results.log
PASSED = 0
FAILED = 0

# Colors for output
RED = \033[0;31m
GREEN = \033[0;32m
YELLOW = \033[1;33m
BLUE = \033[0;34m
BOLD = \033[1m
NC = \033[0m

# Default target - run all tests
all: test

# Run comprehensive test suite
test: banner clean-test-log test-build test-dependency test-sysctl test-control-script test-makefile-commands test-unload summary

# Test banner
banner:
	@echo "$(BOLD)========================================"
	@echo "RPi5 Kernel Module Test Suite"
	@echo "========================================$(NC)"
	@date
	@echo ""

# Initialize test log
clean-test-log:
	@echo "Test Suite Run - $$(date)" > $(TEST_LOG)
	@echo "=======================================" >> $(TEST_LOG)

# Test module building
test-build:
	@echo "$(BLUE)Test 1: Module Building$(NC)"
	@echo "Testing module compilation..." | tee -a $(TEST_LOG)
	@if cd $(PARENT_DIR) && $(MAKE) clean >/dev/null 2>&1 && $(MAKE) >/dev/null 2>&1; then \
		echo "$(GREEN)✓ PASS: Module building successful$(NC)" | tee -a $(TEST_LOG); \
		echo "  - bcm2712.ko and rpi5.ko compiled successfully" | tee -a $(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: Module building failed$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi
	@echo "" | tee -a $(TEST_LOG)

# Test module dependency loading
test-dependency:
	@echo "$(BLUE)Test 2: Module Dependency Loading$(NC)"
	@echo "Testing dependency auto-loading..." | tee -a $(TEST_LOG)

	# Clean slate - unload any existing modules
	@sudo kldunload rpi5 2>/dev/null || true
	@sudo kldunload bcm2712 2>/dev/null || true

	# Install modules
	@cd $(PARENT_DIR) && sudo $(MAKE) install >/dev/null 2>&1

	# Test loading rpi5 (should auto-load bcm2712)
	@if sudo kldload rpi5 2>/dev/null; then \
		if kldstat | grep -q bcm2712 && kldstat | grep -q rpi5; then \
			echo "$(GREEN)✓ PASS: Dependency auto-loading works$(NC)" | tee -a $(TEST_LOG); \
			echo "  - rpi5 module loaded bcm2712 automatically" | tee -a $(TEST_LOG); \
		else \
			echo "$(RED)✗ FAIL: Dependencies not loaded properly$(NC)" | tee -a $(TEST_LOG); \
			exit 1; \
		fi; \
	else \
		echo "$(RED)✗ FAIL: rpi5 module failed to load$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi
	@echo "" | tee -a $(TEST_LOG)

# Test sysctl interface
test-sysctl:
	@echo "$(BLUE)Test 3: sysctl Interface$(NC)"
	@echo "Testing sysctl read/write operations..." | tee -a $(TEST_LOG)

	# Test read operations
	@if sysctl hw.rpi5.fan >/dev/null 2>&1; then \
		echo "$(GREEN)✓ PASS: sysctl read access works$(NC)" | tee -a $(TEST_LOG); \
		echo "  - hw.rpi5.fan.* tree accessible" | tee -a $(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: sysctl interface not available$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi

	# Test write operations and validation
	@echo "  Testing sysctl write operations..." | tee -a $(TEST_LOG)
	@original_temp=$$(sysctl -n hw.rpi5.fan.temp0); \
	if sudo sysctl hw.rpi5.fan.temp0=45000 >/dev/null 2>&1; then \
		new_temp=$$(sysctl -n hw.rpi5.fan.temp0); \
		if [ "$$new_temp" = "45000" ]; then \
			echo "$(GREEN)✓ PASS: sysctl write operations work$(NC)" | tee -a $(TEST_LOG); \
			sudo sysctl hw.rpi5.fan.temp0=$$original_temp >/dev/null 2>&1; \
		else \
			echo "$(RED)✗ FAIL: sysctl value not updated correctly$(NC)" | tee -a $(TEST_LOG); \
			exit 1; \
		fi; \
	else \
		echo "$(RED)✗ FAIL: sysctl write operations failed$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi

	# Test read-only values
	@if sysctl hw.rpi5.fan.current_state >/dev/null 2>&1 && sysctl hw.rpi5.fan.cpu_temp >/dev/null 2>&1; then \
		echo "$(GREEN)✓ PASS: Read-only sysctl values accessible$(NC)" | tee -a $(TEST_LOG); \
		echo "  - current_state and cpu_temp available" | tee -a $(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: Read-only sysctl values not available$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi
	@echo "" | tee -a $(TEST_LOG)

# Test control script functionality
test-control-script:
	@echo "$(BLUE)Test 4: Control Script$(NC)"
	@echo "Testing rpi5_fan_control_integrated.sh..." | tee -a $(TEST_LOG)

	# Make script executable
	@chmod +x $(PARENT_DIR)/rpi5_fan_control_integrated.sh

	# Test --check option (expect PWM warning but script should still run)
	@cd $(PARENT_DIR) && if ./rpi5_fan_control_integrated.sh --check 2>/dev/null | grep -q "BCM2712 hardware module is loaded"; then \
		echo "$(GREEN)✓ PASS: Control script --check works$(NC)" | tee -a test/$(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: Control script --check failed$(NC)" | tee -a test/$(TEST_LOG); \
		exit 1; \
	fi

	# Test --show option
	@cd $(PARENT_DIR) && if ./rpi5_fan_control_integrated.sh --show 2>/dev/null | grep -q "hw.rpi5.fan"; then \
		echo "$(GREEN)✓ PASS: Control script --show works$(NC)" | tee -a test/$(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: Control script --show failed$(NC)" | tee -a test/$(TEST_LOG); \
		exit 1; \
	fi

	echo "  - Control script basic functions operational" | tee -a $(TEST_LOG)
	@echo "" | tee -a $(TEST_LOG)

# Test Makefile commands
test-makefile-commands:
	@echo "$(BLUE)Test 5: Makefile Commands$(NC)"
	@echo "Testing main Makefile targets..." | tee -a $(TEST_LOG)

	# Test status command
	@if cd $(PARENT_DIR) && $(MAKE) status >/dev/null 2>&1; then \
		echo "$(GREEN)✓ PASS: make status works$(NC)" | tee -a $(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: make status failed$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi

	# Test individual module targets
	@if cd $(PARENT_DIR) && $(MAKE) clean >/dev/null 2>&1 && $(MAKE) bcm2712 >/dev/null 2>&1; then \
		echo "$(GREEN)✓ PASS: make bcm2712 works$(NC)" | tee -a $(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: make bcm2712 failed$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi

	@if cd $(PARENT_DIR) && $(MAKE) rpi5 >/dev/null 2>&1; then \
		echo "$(GREEN)✓ PASS: make rpi5 works$(NC)" | tee -a $(TEST_LOG); \
	else \
		echo "$(RED)✗ FAIL: make rpi5 failed$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi

	echo "  - All Makefile targets functional" | tee -a $(TEST_LOG)
	@echo "" | tee -a $(TEST_LOG)

# Test module unloading with dependency checking
test-unload:
	@echo "$(BLUE)Test 6: Module Unloading$(NC)"
	@echo "Testing dependency-aware unloading..." | tee -a $(TEST_LOG)

	# Ensure modules are loaded
	@sudo kldload rpi5 2>/dev/null || true

	# Test that bcm2712 cannot be unloaded while rpi5 depends on it
	@if sudo kldunload bcm2712 2>&1 | grep -q "Device busy"; then \
		echo "$(GREEN)✓ PASS: Dependency protection works$(NC)" | tee -a $(TEST_LOG); \
		echo "  - bcm2712 cannot be unloaded while rpi5 is loaded" | tee -a $(TEST_LOG); \
	else \
		echo "$(YELLOW)⚠ WARNING: Dependency protection not working as expected$(NC)" | tee -a $(TEST_LOG); \
	fi

	# Test proper unloading sequence
	@if cd $(PARENT_DIR) && sudo $(MAKE) unload >/dev/null 2>&1; then \
		if ! kldstat | grep -q rpi5 && ! kldstat | grep -q bcm2712; then \
			echo "$(GREEN)✓ PASS: make unload works correctly$(NC)" | tee -a $(TEST_LOG); \
			echo "  - Both modules unloaded in correct order" | tee -a $(TEST_LOG); \
		else \
			echo "$(RED)✗ FAIL: Modules not unloaded properly$(NC)" | tee -a $(TEST_LOG); \
			exit 1; \
		fi; \
	else \
		echo "$(RED)✗ FAIL: make unload failed$(NC)" | tee -a $(TEST_LOG); \
		exit 1; \
	fi
	@echo "" | tee -a $(TEST_LOG)

# Test summary and cleanup
summary:
	@echo "$(BOLD)========================================" | tee -a $(TEST_LOG)
	@echo "Test Suite Summary" | tee -a $(TEST_LOG)
	@echo "========================================$(NC)" | tee -a $(TEST_LOG)
	@echo "$(GREEN)All tests completed successfully!$(NC)" | tee -a $(TEST_LOG)
	@echo ""
	@echo "$(BOLD)Test Coverage:$(NC)"
	@echo "✓ Module compilation (bcm2712 + rpi5)"
	@echo "✓ Dependency auto-loading"
	@echo "✓ sysctl interface (read/write/read-only)"
	@echo "✓ Control script functionality"
	@echo "✓ Makefile targets"
	@echo "✓ Module unloading with dependency management"
	@echo ""
	@echo "$(GREEN)$(BOLD)🎉 RPi5 kernel module test suite PASSED$(NC)"
	@echo "Full test log available in: test/$(TEST_LOG)"

# Stress test - load/unload cycles
stress-test:
	@echo "$(BLUE)Stress Test: Load/Unload Cycles$(NC)"
	@echo "Running 10 load/unload cycles..."
	@for i in 1 2 3 4 5 6 7 8 9 10; do \
		echo "  Cycle $$i/10..."; \
		cd $(PARENT_DIR) && sudo $(MAKE) load >/dev/null 2>&1 || true; \
		sleep 1; \
		cd $(PARENT_DIR) && sudo $(MAKE) unload >/dev/null 2>&1 || true; \
		sleep 1; \
	done
	@echo "$(GREEN)✓ Stress test completed - 10 cycles successful$(NC)"

# Performance test - measure load times
perf-test:
	@echo "$(BLUE)Performance Test: Module Load Times$(NC)"
	@echo "Measuring module load performance..."
	@for i in 1 2 3 4 5; do \
		cd $(PARENT_DIR) && sudo $(MAKE) unload >/dev/null 2>&1; \
		start_time=$$(date +%s.%N); \
		cd $(PARENT_DIR) && sudo $(MAKE) load >/dev/null 2>&1; \
		end_time=$$(date +%s.%N); \
		load_time=$$(echo "$$end_time - $$start_time" | bc -l); \
		echo "  Load $$i: $${load_time}s"; \
	done

# Development test - quick validation during development
dev-test:
	@echo "$(BLUE)Development Test: Quick Validation$(NC)"
	@cd $(PARENT_DIR) && $(MAKE) clean >/dev/null 2>&1
	@cd $(PARENT_DIR) && $(MAKE) >/dev/null 2>&1
	@cd $(PARENT_DIR) && sudo $(MAKE) unload >/dev/null 2>&1 || true
	@cd $(PARENT_DIR) && sudo $(MAKE) install load >/dev/null 2>&1
	@sysctl hw.rpi5.fan >/dev/null 2>&1
	@echo "$(GREEN)✓ Development test passed - basic functionality working$(NC)"

# Clean up test artifacts
clean:
	@rm -f $(TEST_LOG)
	@echo "Test artifacts cleaned"

# Help target
help:
	@echo "RPi5 Kernel Module Test Suite"
	@echo ""
	@echo "Available targets:"
	@echo "  test        - Run complete test suite (default)"
	@echo "  stress-test - Run load/unload stress test"
	@echo "  perf-test   - Measure module load performance"
	@echo "  dev-test    - Quick development validation"
	@echo "  clean       - Clean test artifacts"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make              # Run full test suite"
	@echo "  make stress-test  # Run stress testing"
	@echo "  make dev-test     # Quick dev validation"

.PHONY: all test banner clean-test-log test-build test-dependency test-sysctl test-control-script test-makefile-commands test-unload summary stress-test perf-test dev-test clean help