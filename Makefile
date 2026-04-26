# Raspberry Pi 5 Hardware Support Modules - Consolidated Makefile
# SPDX-License-Identifier: BSD-2-Clause

# Default target - build all modules
all: bcm2712 rpi5 rp1_eth rp1_pcie2_recon bcm2712_pcie

# BCM2712 common hardware module target
bcm2712:
	@echo "Building BCM2712 common hardware module..."
	$(MAKE) -f Makefile.bcm2712

# RPi5 board-specific module target
rpi5:
	@echo "Building RPi5 board-specific module..."
	$(MAKE) -f Makefile.rpi5

# RP1 Ethernet milestone 1 module target
rp1_eth:
	@echo "Building RP1 Ethernet (Milestone 1) module..."
	$(MAKE) -f Makefile.rp1_eth

# PCIe2 reconnaissance module target (Milestone 3 prep)
rp1_pcie2_recon:
	@echo "Building RP1 PCIe2 reconnaissance module..."
	$(MAKE) -f Makefile.rp1_pcie2_recon

# BCM2712 PCIe2/RP1 interrupt router (Milestone 3)
bcm2712_pcie:
	@echo "Building BCM2712 PCIe2/RP1 interrupt router..."
	$(MAKE) -f Makefile.bcm2712_pcie

# Install both modules
install: install-bcm2712 install-rpi5 install-rp1_eth install-rp1_pcie2_recon install-bcm2712_pcie

# Install BCM2712 common hardware module
install-bcm2712:
	@echo "Installing BCM2712 common hardware module..."
	$(MAKE) -f Makefile.bcm2712 install

# Install RPi5 board-specific module
install-rpi5:
	@echo "Installing RPi5 board-specific module..."
	$(MAKE) -f Makefile.rpi5 install

# Install RP1 Ethernet module
install-rp1_eth:
	@echo "Installing RP1 Ethernet module..."
	$(MAKE) -f Makefile.rp1_eth install

# Install PCIe2 recon module
install-rp1_pcie2_recon:
	@echo "Installing RP1 PCIe2 reconnaissance module..."
	$(MAKE) -f Makefile.rp1_pcie2_recon install

# Install BCM2712 PCIe interrupt router
install-bcm2712_pcie:
	@echo "Installing BCM2712 PCIe2/RP1 interrupt router..."
	$(MAKE) -f Makefile.bcm2712_pcie install

# Clean all build artifacts
clean: clean-bcm2712 clean-rpi5 clean-rp1_eth clean-rp1_pcie2_recon clean-bcm2712_pcie

# Clean BCM2712 module build artifacts
clean-bcm2712:
	@echo "Cleaning BCM2712 common hardware build artifacts..."
	$(MAKE) -f Makefile.bcm2712 clean

# Clean RPi5 module build artifacts
clean-rpi5:
	@echo "Cleaning RPi5 board-specific build artifacts..."
	$(MAKE) -f Makefile.rpi5 clean

# Clean RP1 Ethernet module build artifacts
clean-rp1_eth:
	@echo "Cleaning RP1 Ethernet module build artifacts..."
	$(MAKE) -f Makefile.rp1_eth clean

# Clean PCIe2 recon module build artifacts
clean-rp1_pcie2_recon:
	@echo "Cleaning RP1 PCIe2 reconnaissance module build artifacts..."
	$(MAKE) -f Makefile.rp1_pcie2_recon clean

# Clean BCM2712 PCIe interrupt router build artifacts
clean-bcm2712_pcie:
	@echo "Cleaning BCM2712 PCIe2/RP1 interrupt router build artifacts..."
	$(MAKE) -f Makefile.bcm2712_pcie clean

# Load all modules (requires root)
load: load-bcm2712 load-rpi5 load-rp1_eth

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

# Load RP1 Ethernet module (depends on bcm2712)
load-rp1_eth:
	@echo "Loading RP1 Ethernet module..."
	@if kldstat | grep -q rp1_eth; then \
		echo "rp1_eth module already loaded"; \
	else \
		kldload rp1_eth; \
		echo "rp1_eth module loaded"; \
	fi

# Load PCIe2 recon module
load-rp1_pcie2_recon:
	@echo "Loading RP1 PCIe2 reconnaissance module..."
	@if kldstat | grep -q rp1_pcie2_recon; then \
		echo "rp1_pcie2_recon module already loaded"; \
	else \
		kldload rp1_pcie2_recon; \
		echo "rp1_pcie2_recon module loaded"; \
	fi

# Unload all modules (requires root; rp1_eth before bcm2712)
unload: unload-rp1_pcie2_recon unload-rp1_eth unload-rpi5 unload-bcm2712

# Unload RPi5 module (must unload first due to dependency)
unload-rpi5:
	@echo "Unloading RPi5 board-specific module..."
	@if kldstat | grep -q rpi5; then \
		kldunload rpi5; \
		echo "RPi5 module unloaded"; \
	else \
		echo "RPi5 module not loaded"; \
	fi

# Unload PCIe2 recon module
unload-rp1_pcie2_recon:
	@echo "Unloading RP1 PCIe2 reconnaissance module..."
	@if kldstat | grep -q rp1_pcie2_recon; then \
		kldunload rp1_pcie2_recon; \
		echo "rp1_pcie2_recon module unloaded"; \
	else \
		echo "rp1_pcie2_recon module not loaded"; \
	fi

# Unload RP1 Ethernet module
unload-rp1_eth:
	@echo "Unloading RP1 Ethernet module..."
	@if kldstat | grep -q rp1_eth; then \
		kldunload rp1_eth; \
		echo "rp1_eth module unloaded"; \
	else \
		echo "rp1_eth module not loaded"; \
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
	@echo "RP1 Ethernet (Milestone 1):"
	@if kldstat | grep -q rp1_eth; then \
		echo "  ✓ rp1_eth is loaded"; \
	else \
		echo "  ✗ rp1_eth is NOT loaded"; \
	fi
	@echo ""
	@echo "=== sysctl Interface ==="
	@if sysctl hw.rpi5.fan >/dev/null 2>&1; then \
		echo "  ✓ hw.rpi5 sysctl available"; \
		sysctl hw.rpi5.fan.current_state hw.rpi5.fan.cpu_temp; \
	else \
		echo "  ✗ hw.rpi5 sysctl NOT available"; \
	fi
	@if sysctl hw.rp1_eth.cfg.status_decoded >/dev/null 2>&1; then \
		echo "  ✓ hw.rp1_eth sysctl available"; \
		sysctl hw.rp1_eth.cfg.status_decoded; \
	else \
		echo "  ✗ hw.rp1_eth sysctl NOT available"; \
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

.PHONY: all bcm2712 rpi5 rp1_eth rp1_pcie2_recon bcm2712_pcie install install-bcm2712 install-rpi5 install-rp1_eth install-rp1_pcie2_recon install-bcm2712_pcie clean clean-bcm2712 clean-rpi5 clean-rp1_eth clean-rp1_pcie2_recon clean-bcm2712_pcie load load-bcm2712 load-rpi5 load-rp1_eth load-rp1_pcie2_recon unload unload-bcm2712 unload-rpi5 unload-rp1_eth unload-rp1_pcie2_recon status test test-suite dev-test stress-test help