# Raspberry Pi 5 Hardware Support Modules - Consolidated Makefile
# SPDX-License-Identifier: BSD-2-Clause

# Default target - build all modules
all: bcm2712 rpi5

# BCM2712 common hardware module target
bcm2712:
	@echo "Building BCM2712 common hardware module..."
	$(MAKE) -f Makefile.bcm2712

# RPi5 board-specific module target
rpi5:
	@echo "Building RPi5 board-specific module..."
	$(MAKE) -f Makefile.rpi5

# Install both modules
install: install-bcm2712 install-rpi5

# Install BCM2712 common hardware module
install-bcm2712:
	@echo "Installing BCM2712 common hardware module..."
	$(MAKE) -f Makefile.bcm2712 install

# Install RPi5 board-specific module
install-rpi5:
	@echo "Installing RPi5 board-specific module..."
	$(MAKE) -f Makefile.rpi5 install

# Clean all build artifacts
clean: clean-bcm2712 clean-rpi5

# Clean BCM2712 module build artifacts
clean-bcm2712:
	@echo "Cleaning BCM2712 common hardware build artifacts..."
	$(MAKE) -f Makefile.bcm2712 clean

# Clean RPi5 module build artifacts
clean-rpi5:
	@echo "Cleaning RPi5 board-specific build artifacts..."
	$(MAKE) -f Makefile.rpi5 clean

# Load both modules (requires root)
load: load-bcm2712 load-rpi5

# Load BCM2712 common hardware module
load-bcm2712:
	@echo "Loading BCM2712 common hardware module..."
	@if kldstat | grep -q bcm2712; then \
		echo "BCM2712 module already loaded"; \
	else \
		kldload bcm2712; \
		echo "BCM2712 module loaded"; \
	fi

# Load RPi5 board-specific module (depends on bcm2712)
load-rpi5:
	@echo "Loading RPi5 board-specific module..."
	@if kldstat | grep -q rpi5; then \
		echo "RPi5 module already loaded"; \
	else \
		kldload rpi5; \
		echo "RPi5 module loaded (bcm2712 auto-loaded)"; \
	fi

# Unload both modules (requires root)
unload: unload-rpi5 unload-bcm2712

# Unload RPi5 module (must unload first due to dependency)
unload-rpi5:
	@echo "Unloading RPi5 board-specific module..."
	@if kldstat | grep -q rpi5; then \
		kldunload rpi5; \
		echo "RPi5 module unloaded"; \
	else \
		echo "RPi5 module not loaded"; \
	fi

# Unload BCM2712 common hardware module
unload-bcm2712:
	@echo "Unloading BCM2712 common hardware module..."
	@if kldstat | grep -q bcm2712; then \
		kldunload bcm2712; \
		echo "BCM2712 module unloaded"; \
	else \
		echo "BCM2712 module not loaded"; \
	fi

# Check module status
status:
	@echo "=== Module Status ==="
	@echo "BCM2712 Common Hardware:"
	@if kldstat | grep -q bcm2712; then \
		echo "  ✓ bcm2712 is loaded"; \
	else \
		echo "  ✗ bcm2712 is NOT loaded"; \
	fi
	@echo "RPi5 Board Support:"
	@if kldstat | grep -q rpi5; then \
		echo "  ✓ rpi5 is loaded"; \
	else \
		echo "  ✗ rpi5 is NOT loaded"; \
	fi
	@echo ""
	@echo "=== sysctl Interface ==="
	@if sysctl hw.rpi5.fan >/dev/null 2>&1; then \
		echo "  ✓ sysctl interface available"; \
		sysctl hw.rpi5.fan.current_state hw.rpi5.fan.cpu_temp; \
	else \
		echo "  ✗ sysctl interface NOT available"; \
	fi

# Test the system (build, install, load, and check status)
test: all install load status

# Run comprehensive test suite (non-default, explicit target)
test-suite:
	@echo "Running comprehensive test suite..."
	@$(MAKE) -C test

# Quick development test
dev-test:
	@echo "Running quick development test..."
	@$(MAKE) -C test dev-test

# Stress testing
stress-test:
	@echo "Running stress test..."
	@$(MAKE) -C test stress-test

# Help target
help:
	@echo "Raspberry Pi 5 Cooling Fan Control Modules - Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  all          - Build all modules (default)"
	@echo "  bcm2712      - Build BCM2712 common hardware module only"
	@echo "  rpi5         - Build RPi5 board-specific module only"
	@echo ""
	@echo "  install      - Install all modules"
	@echo "  install-bcm2712 - Install BCM2712 common hardware module only"
	@echo "  install-rpi5 - Install RPi5 board-specific module only"
	@echo ""
	@echo "  clean        - Clean all build artifacts"
	@echo "  clean-bcm2712 - Clean BCM2712 module build artifacts"
	@echo "  clean-rpi5   - Clean RPi5 module build artifacts"
	@echo ""
	@echo "  load         - Load all modules (requires root)"
	@echo "  load-bcm2712 - Load BCM2712 common hardware module (requires root)"
	@echo "  load-rpi5    - Load RPi5 board-specific module (requires root)"
	@echo ""
	@echo "  unload       - Unload all modules (requires root)"
	@echo "  unload-bcm2712 - Unload BCM2712 common hardware module (requires root)"
	@echo "  unload-rpi5  - Unload RPi5 board-specific module (requires root)"
	@echo ""
	@echo "  status       - Check module load status and sysctl interface"
	@echo "  test         - Build, install, load, and check status"
	@echo "  test-suite   - Run comprehensive test suite"
	@echo "  dev-test     - Quick development validation"
	@echo "  stress-test  - Run load/unload stress testing"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build all modules"
	@echo "  sudo make install       # Install all modules"
	@echo "  sudo make load          # Load all modules"
	@echo "  make status             # Check status"
	@echo "  sudo make test          # Full build, install, and test"
	@echo "  make test-suite         # Run comprehensive test suite"
	@echo "  make clean              # Clean build artifacts"

.PHONY: all bcm2712 rpi5 install install-bcm2712 install-rpi5 clean clean-bcm2712 clean-rpi5 load load-bcm2712 load-rpi5 unload unload-bcm2712 unload-rpi5 status test test-suite dev-test stress-test help