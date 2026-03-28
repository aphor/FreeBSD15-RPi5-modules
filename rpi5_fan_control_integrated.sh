#!/bin/sh
#
# RPi5 Cooling Fan Control Script with PWM Driver Integration
# Works with both the RP1 PWM driver and thermal management module

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Helper functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

# Check if modules are loaded
check_modules() {
    log_info "Checking kernel modules..."
    
    local pwm_loaded=0
    local thermal_loaded=0
    
    if kldstat | grep -q "rp1_pwm"; then
        log_info "RP1 PWM driver is loaded"
        pwm_loaded=1
    else
        log_warn "RP1 PWM driver is NOT loaded"
    fi
    
    if kldstat | grep -q "rpi5_cooling_fan"; then
        log_info "Thermal management module is loaded"
        thermal_loaded=1
    else
        log_warn "Thermal management module is NOT loaded"
    fi
    
    if [ $pwm_loaded -eq 0 ] || [ $thermal_loaded -eq 0 ]; then
        log_error "Some required modules are not loaded"
        return 1
    fi
    
    return 0
}

# Check PWM device availability
check_pwm_device() {
    log_info "Checking PWM device..."
    
    if [ -e "/dev/pwm/pwmc2.3" ]; then
        log_info "Fan PWM device found at /dev/pwm/pwmc2.3"
        return 0
    elif [ -e "/dev/pwm/pwmc3.3" ]; then
        log_info "Fan PWM device found at /dev/pwm/pwmc3.3"
        return 0
    else
        log_warn "Fan PWM device not found in /dev/pwm/"
        log_debug "Available PWM devices:"
        ls -la /dev/pwm/ 2>/dev/null || log_warn "No /dev/pwm directory"
        return 1
    fi
}

# Display all cooling fan settings
show_all_settings() {
    log_info "Current Cooling Fan Settings:"
    echo "======================================="
    echo "Temperature Thresholds (milli-°C):"
    sysctl hw.rpi5.cooling_fan.temp0
    sysctl hw.rpi5.cooling_fan.temp1
    sysctl hw.rpi5.cooling_fan.temp2
    sysctl hw.rpi5.cooling_fan.temp3
    echo ""
    echo "Hysteresis Values (milli-°C):"
    sysctl hw.rpi5.cooling_fan.temp0_hyst
    sysctl hw.rpi5.cooling_fan.temp1_hyst
    sysctl hw.rpi5.cooling_fan.temp2_hyst
    sysctl hw.rpi5.cooling_fan.temp3_hyst
    echo ""
    echo "PWM Speed Levels (0-255):"
    sysctl hw.rpi5.cooling_fan.speed0
    sysctl hw.rpi5.cooling_fan.speed1
    sysctl hw.rpi5.cooling_fan.speed2
    sysctl hw.rpi5.cooling_fan.speed3
    echo ""
    echo "Current Status:"
    sysctl hw.rpi5.cooling_fan.cpu_temp
    sysctl hw.rpi5.cooling_fan.current_state
    echo "======================================="
}

# Run full system diagnostics
run_diagnostics() {
    log_info "Running system diagnostics..."
    echo ""
    
    echo "=== Module Status ==="
    check_modules || log_warn "Module check failed"
    echo ""
    
    echo "=== PWM Device Check ==="
    check_pwm_device || log_warn "PWM device check failed"
    echo ""
    
    echo "=== Kernel Messages ==="
    log_debug "Recent kernel messages related to rp1/pwm:"
    dmesg | tail -20 | grep -E "(rp1|pwm|thermal)" || log_debug "No recent rp1/pwm messages"
    echo ""
    
    echo "=== Current Configuration ==="
    show_all_settings
    echo ""
    
    echo "=== PWM Bus Information ==="
    if command -v pwm >/dev/null 2>&1; then
        log_debug "PWM command is available"
        pwm list 2>/dev/null || log_warn "Could not list PWM devices"
    else
        log_debug "PWM command not available (can be installed from sysutils/pwm)"
    fi
    echo ""
    
    log_info "Diagnostics complete"
}

# Set aggressive cooling profile
aggressive_cooling() {
    log_info "Setting aggressive cooling profile..."
    log_warn "This will turn on the fan at lower temperatures"
    
    sysctl hw.rpi5.cooling_fan.temp0=40000  # 40°C
    sysctl hw.rpi5.cooling_fan.temp1=50000  # 50°C
    sysctl hw.rpi5.cooling_fan.temp2=60000  # 60°C
    sysctl hw.rpi5.cooling_fan.temp3=70000  # 70°C
    
    sysctl hw.rpi5.cooling_fan.speed0=150
    sysctl hw.rpi5.cooling_fan.speed1=180
    sysctl hw.rpi5.cooling_fan.speed2=220
    sysctl hw.rpi5.cooling_fan.speed3=255
    
    log_info "Aggressive cooling profile applied"
}

# Set quiet cooling profile
quiet_cooling() {
    log_info "Setting quiet cooling profile..."
    log_warn "Fan will run less frequently but may tolerate higher temperatures"
    
    sysctl hw.rpi5.cooling_fan.temp0=60000  # 60°C
    sysctl hw.rpi5.cooling_fan.temp1=70000  # 70°C
    sysctl hw.rpi5.cooling_fan.temp2=80000  # 80°C
    sysctl hw.rpi5.cooling_fan.temp3=90000  # 90°C
    
    sysctl hw.rpi5.cooling_fan.speed0=50
    sysctl hw.rpi5.cooling_fan.speed1=100
    sysctl hw.rpi5.cooling_fan.speed2=150
    sysctl hw.rpi5.cooling_fan.speed3=255
    
    log_info "Quiet cooling profile applied"
}

# Reset to default settings
reset_defaults() {
    log_info "Resetting to default settings..."
    
    sysctl hw.rpi5.cooling_fan.temp0=50000
    sysctl hw.rpi5.cooling_fan.temp1=60000
    sysctl hw.rpi5.cooling_fan.temp2=67500
    sysctl hw.rpi5.cooling_fan.temp3=75000
    
    sysctl hw.rpi5.cooling_fan.temp0_hyst=5000
    sysctl hw.rpi5.cooling_fan.temp1_hyst=5000
    sysctl hw.rpi5.cooling_fan.temp2_hyst=5000
    sysctl hw.rpi5.cooling_fan.temp3_hyst=5000
    
    sysctl hw.rpi5.cooling_fan.speed0=75
    sysctl hw.rpi5.cooling_fan.speed1=125
    sysctl hw.rpi5.cooling_fan.speed2=175
    sysctl hw.rpi5.cooling_fan.speed3=250
    
    log_info "Default settings restored"
}

# Monitor current temperature and fan state
monitor_fan() {
    log_info "Monitoring cooling fan state (Ctrl+C to stop)..."
    log_info "Refreshing every 1 second..."
    echo ""
    
    while true; do
        local state=$(sysctl -n hw.rpi5.cooling_fan.current_state 2>/dev/null || echo "?")
        local temp_mc=$(sysctl -n hw.rpi5.cooling_fan.cpu_temp 2>/dev/null || echo "?")
        local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
        
        # Convert millicelsius to celsius for display
        if [ "$temp_mc" != "?" ]; then
            local temp_c=$((temp_mc / 1000))
            local temp_f=$((temp_c * 9 / 5 + 32))
            printf "%s | State: %s | Temp: %d°C (%d°F)\n" "$timestamp" "$state" "$temp_c" "$temp_f"
        else
            printf "%s | State: %s | Temp: ? °C\n" "$timestamp" "$state"
        fi
        
        sleep 1
    done
}

# Detailed test of PWM control
test_pwm() {
    log_info "Testing PWM hardware control..."
    
    if ! check_modules; then
        log_error "Required modules not loaded"
        return 1
    fi
    
    if ! check_pwm_device; then
        log_error "PWM device not found"
        return 1
    fi
    
    log_info "Testing PWM speed levels..."
    
    # Test different speed levels
    for speed in 0 75 125 175 250; do
        log_info "Setting speed to $speed (0-255)..."
        sysctl hw.rpi5.cooling_fan.speed0=$speed > /dev/null
        sleep 1
    done
    
    log_info "PWM test complete"
}

# Load modules if not already loaded
load_modules() {
    if ! kldstat | grep -q "rp1_pwm"; then
        log_info "Loading RP1 PWM driver..."
        sudo kldload rp1_pwm || log_error "Failed to load rp1_pwm"
    fi
    
    if ! kldstat | grep -q "rpi5_cooling_fan"; then
        log_info "Loading thermal management module..."
        sudo kldload rpi5_cooling_fan || log_error "Failed to load rpi5_cooling_fan"
    fi
    
    check_modules
}

# Show help
show_help() {
    cat << EOF
Usage: $(basename $0) [OPTION]

Options:
    -h, --help              Show this help message
    -c, --check             Check module and PWM device status
    -d, --diagnostics       Run full system diagnostics
    -s, --show              Show all current settings
    -a, --aggressive        Set aggressive cooling profile
    -q, --quiet             Set quiet cooling profile
    -r, --reset             Reset to default settings
    -m, --monitor           Monitor fan state in real-time
    -t, --test              Test PWM hardware control
    -l, --load              Load kernel modules

Examples:
    # Check system status
    $(basename $0) --check

    # Run diagnostics
    $(basename $0) --diagnostics

    # View all settings
    $(basename $0) --show

    # Switch to aggressive cooling
    $(basename $0) --aggressive

    # Monitor fan in real-time
    $(basename $0) --monitor

For manual control, use sysctl directly:
    sysctl hw.rpi5.cooling_fan.temp0=45000
    sysctl hw.rpi5.cooling_fan.speed0=100

EOF
}

# Main script logic
main() {
    if [ $# -eq 0 ]; then
        show_help
        exit 0
    fi
    
    case "$1" in
        -h|--help)
            show_help
            ;;
        -c|--check)
            check_modules && check_pwm_device
            ;;
        -d|--diagnostics)
            run_diagnostics
            ;;
        -s|--show)
            check_modules && show_all_settings
            ;;
        -a|--aggressive)
            check_modules && aggressive_cooling && show_all_settings
            ;;
        -q|--quiet)
            check_modules && quiet_cooling && show_all_settings
            ;;
        -r|--reset)
            check_modules && reset_defaults && show_all_settings
            ;;
        -m|--monitor)
            check_modules && monitor_fan
            ;;
        -t|--test)
            test_pwm
            ;;
        -l|--load)
            load_modules
            ;;
        *)
            log_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
}

# Run main function
main "$@"
