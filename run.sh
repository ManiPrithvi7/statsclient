#!/bin/bash
# ESP32 WiFi Provisioning - Build, Flash, and Test Script
# This single script handles everything: build → flash → connect → test

set -euo pipefail

# Configuration
ESP_PORT="/dev/ttyACM0"
AP_SSID="ESP32-Prov"
AP_PASSWORD="prov12345678"
ESP_IP="192.168.4.1"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Timeouts (override via env if desired)
: "${IDF_BUILD_TIMEOUT:=20m}"
: "${IDF_FLASH_TIMEOUT:=10m}"
: "${CURL_TIMEOUT:=30}"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Helper functions
print_step() {
    echo -e "\n${GREEN}>>> $1${NC}"
}

print_info() {
    echo -e "${YELLOW}ℹ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

# Step 0: Cleanup processes using serial port
cleanup_serial_port() {
    print_step "Cleaning up serial port"
    
    # Check if port exists
    if [ ! -e "$ESP_PORT" ]; then
        print_info "Port $ESP_PORT does not exist yet (device may not be connected)"
        return 0
    fi
    
    # Check if port is in use
    if lsof "$ESP_PORT" >/dev/null 2>&1; then
        print_info "Serial port is in use. Cleaning up processes..."
        
        # Kill common processes that might lock the port
        pkill -f "idf.py.*monitor" 2>/dev/null && print_info "  ✓ Killed idf.py monitor processes"
        pkill -f "esp_idf_monitor" 2>/dev/null && print_info "  ✓ Killed esp_idf_monitor processes"
        pkill -f "idf_monitor" 2>/dev/null && print_info "  ✓ Killed idf_monitor processes"
        
        # Use fuser as fallback
        if fuser -k "$ESP_PORT" >/dev/null 2>&1; then
            print_info "  ✓ Killed processes using port via fuser"
        fi
        
        # Wait for processes to die
        sleep 2
        
        # Verify port is free
        if lsof "$ESP_PORT" >/dev/null 2>&1; then
            print_warning "Some processes may still be using the port"
            print_info "Attempting to continue anyway..."
        else
            print_success "Port is now free"
        fi
    else
        print_success "Port is already free"
    fi
    
    # Also clean up any build artifacts that might cause issues
    print_info "Cleaning up build artifacts..."
    cd "$PROJECT_DIR"
    if [ -d "build" ]; then
        # Don't remove build directory, just clean stale locks
        rm -f build/.cmake_lock 2>/dev/null
        print_info "  ✓ Cleaned build locks"
    fi
}

# Step 1: Setup ESP-IDF environment
setup_esp_idf() {
    print_step "Setting up ESP-IDF environment"
    if [ -f "$PROJECT_DIR/esp-idf/export.sh" ]; then
        # shellcheck disable=SC1091
        source "$PROJECT_DIR/esp-idf/export.sh" >/dev/null 2>&1
        print_success "ESP-IDF environment loaded"
    else
        print_error "ESP-IDF not found. Please install ESP-IDF first."
        exit 1
    fi

    if ! command -v idf.py >/dev/null 2>&1; then
        print_error "idf.py not found in PATH after sourcing ESP-IDF environment"
        print_info "Expected: source \"$PROJECT_DIR/esp-idf/export.sh\" to provide idf.py"
        exit 1
    fi
}

# Step 2: Build project
build_project() {
    print_step "Building project"
    cd "$PROJECT_DIR"
    
    local build_log
    build_log="$(mktemp)"
    print_info "Running build (timeout: $IDF_BUILD_TIMEOUT)..."

    # Avoid command-substitution + set -e interactions; stream output live and keep a log.
    set +e
    timeout "$IDF_BUILD_TIMEOUT" idf.py build 2>&1 | tee "$build_log"
    local build_exit=${PIPESTATUS[0]}
    set -e
    
    # Check if build directory is misconfigured (configured for different project)
    if grep -qi "configured for project.*not" "$build_log"; then
        print_warning "Build directory misconfigured for different project, cleaning..."
        idf.py fullclean >/dev/null 2>&1
        print_info "Build directory cleaned, rebuilding..."
        set +e
        timeout "$IDF_BUILD_TIMEOUT" idf.py build 2>&1 | tee "$build_log"
        build_exit=${PIPESTATUS[0]}
        set -e
    fi

    # Recover from corrupted build metadata (common after interrupted builds)
    if [ "$build_exit" -ne 0 ] && (grep -q "Failed to read component info from project_description.json" "$build_log" || grep -q "JSONDecodeError" "$build_log"); then
        print_warning "Build metadata appears corrupted (project_description.json). Running fullclean and rebuilding..."
        idf.py fullclean >/dev/null 2>&1
        print_info "Rebuilding after fullclean..."
        set +e
        timeout "$IDF_BUILD_TIMEOUT" idf.py build 2>&1 | tee "$build_log"
        build_exit=${PIPESTATUS[0]}
        set -e
    fi
    
    if [ "$build_exit" -eq 0 ]; then
        print_success "Build completed"
        rm -f "$build_log"
    else
        # Show actual build errors
        print_error "Build failed. Showing errors:"
        tail -50 "$build_log" || true
        print_info "Full build log: $build_log"
        exit 1
    fi
}

# Step 3: Flash to ESP32
flash_esp32() {
    print_step "Flashing firmware to ESP32"
    cd "$PROJECT_DIR"
    
    # Quick check if port is still in use (cleanup should have handled this, but double-check)
    if lsof "$ESP_PORT" >/dev/null 2>&1; then
        print_warning "Port is still in use, attempting quick cleanup..."
        pkill -9 -f "idf.py.*monitor" 2>/dev/null
        pkill -9 -f "esp_idf_monitor" 2>/dev/null
        fuser -k "$ESP_PORT" 2>/dev/null
        sleep 1
    fi
    
    if [ ! -e "$ESP_PORT" ]; then
        print_error "ESP32 not found at $ESP_PORT"
        print_info "Available ports:"
        ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "  None found"
        exit 1
    fi
    
    # Check port permissions
    if [ ! -r "$ESP_PORT" ] || [ ! -w "$ESP_PORT" ]; then
        print_error "Port $ESP_PORT is not readable/writable"
        print_info "Attempting to fix permissions..."
        
        # Try to fix permissions
        if sudo chmod 666 "$ESP_PORT" 2>/dev/null; then
            print_success "Permissions fixed temporarily"
        else
            print_error "Cannot fix permissions automatically"
            echo ""
            print_info "Please run ONE of these commands:"
            echo ""
            echo "  Option 1 (Temporary fix - run each time):"
            echo "    sudo chmod 666 $ESP_PORT"
            echo ""
            echo "  Option 2 (Permanent fix - recommended):"
            echo "    sudo usermod -a -G dialout $USER"
            echo "    (Then logout and login again)"
            echo ""
            print_info "After fixing, run this script again: ./run.sh"
            exit 1
        fi
    fi
    
    # Try to flash (show errors for debugging)
    print_info "Flashing to $ESP_PORT..."
    local flash_log
    flash_log="$(mktemp)"
    set +e
    timeout "$IDF_FLASH_TIMEOUT" idf.py -p "$ESP_PORT" flash >"$flash_log" 2>&1
    local flash_exit=$?
    set -e

    if [ "$flash_exit" -eq 0 ]; then
        # Check for success indicators
        if grep -q "Hash of data verified" "$flash_log" || grep -q "Leaving" "$flash_log"; then
            print_success "Firmware flashed successfully"
            rm -f "$flash_log"
        else
            print_error "Flash may have failed - no success indicator found"
            print_info "Flash output:"
            tail -40 "$flash_log" || true
            print_info "Full flash log: $flash_log"
            exit 1
        fi
    else
        print_error "Flash failed"
        print_info "Flash output:"
        tail -80 "$flash_log" || true
        print_info "Full flash log: $flash_log"
        exit 1
    fi
}

# Step 4: Wait for ESP32 to boot
wait_for_boot() {
    print_step "Waiting for ESP32 to boot and initialize WiFi (30 seconds)"
    print_info "ESP32 needs time to: boot → initialize NVS → start WiFi AP → start HTTP server"
    print_info "If AP doesn't appear, device may need a manual reset (press RESET button)"
    
    for i in {30..1}; do
        echo -ne "\r  Boot countdown: $i seconds... "
        sleep 1
    done
    echo -e "\r  Boot countdown: 0 seconds... Done"
    print_info "Device should be ready now"
    
    # Optional: Try to reset the device via esptool
    print_info "Attempting to reset device..."
    if command -v esptool.py >/dev/null 2>&1 || python -m esptool >/dev/null 2>&1; then
        python -m esptool --chip esp32s3 --port "$ESP_PORT" run 2>/dev/null && print_success "Device reset" || print_info "Reset attempted (may need manual reset button)"
    fi
    sleep 2
}

# Step 5: Connect to ESP32 AP
connect_to_ap() {
    print_step "Connecting to ESP32 Access Point"
    
    # Check if already connected
    if ping -c 1 -W 2 "$ESP_IP" >/dev/null 2>&1; then
        print_success "Already connected to ESP32 AP"
        return 0
    fi
    
    # Scan for AP with retries
    print_info "Scanning for ESP32 AP (this may take a few attempts)..."
    MAX_RETRIES=5
    RETRY_COUNT=0
    
    while [ $RETRY_COUNT -lt $MAX_RETRIES ]; do
        nmcli device wifi rescan >/dev/null 2>&1
        sleep 3
        
        if nmcli device wifi list | grep -qi "$AP_SSID"; then
            print_success "ESP32 AP found!"
            break
        fi
        
        RETRY_COUNT=$((RETRY_COUNT + 1))
        if [ $RETRY_COUNT -lt $MAX_RETRIES ]; then
            print_info "Attempt $RETRY_COUNT/$MAX_RETRIES: AP not found yet, retrying..."
        fi
    done
    
    if [ $RETRY_COUNT -eq $MAX_RETRIES ]; then
        print_warning "ESP32 AP '$AP_SSID' not found after $MAX_RETRIES attempts"
        print_info "Possible reasons:"
        print_info "  1. Device needs more time to boot"
        print_info "  2. Device error during boot"
        print_info "  3. Device is reusing persisted credentials and skipped AP mode"
        return 2
    fi
    
    # Connect to AP
    print_info "Connecting to $AP_SSID..."
    if nmcli device wifi connect "$AP_SSID" password "$AP_PASSWORD" >/dev/null 2>&1; then
        sleep 3
        if ping -c 1 -W 2 "$ESP_IP" >/dev/null 2>&1; then
            print_success "Connected to ESP32 AP"
            return 0
        fi
    fi
    
    print_error "Failed to connect to ESP32 AP"
    return 1
}

# Step 6: Captive portal provisioning flow
wait_for_http_provisioning() {
    print_step "WiFi Provisioning via Captive Portal"
    : "${PROVISIONING_WAIT_TIMEOUT:=300}" # seconds

    # Verify ESP32 portal is reachable
    print_info "Verifying ESP32 provisioning portal..."
    if STATUS=$(curl -s --max-time 5 "http://$ESP_IP/status" 2>/dev/null); then
        if echo "$STATUS" | grep -q "status"; then
            print_success "ESP32 portal is reachable"
        else
            print_error "ESP32 not responding correctly on /status"
            return 1
        fi
    else
        print_error "Cannot reach ESP32 at $ESP_IP"
        return 1
    fi

    echo ""
    print_info "========================================"
    print_info "Use Web Portal (new flow)"
    print_info "========================================"
    print_info "1) Keep a phone/laptop connected to: $AP_SSID"
    print_info "2) Open: http://proof-setup.local/  (fallback: http://$ESP_IP/)"
    print_info "3) Paste provisioning token in portal URL/field"
    print_info "4) Submit WiFi credentials from the portal"
    print_info ""
    print_info "Script will wait up to ${PROVISIONING_WAIT_TIMEOUT}s for transition."
    print_info "Success indicators:"
    print_info "  - /status returns connected"
    print_info "  - OR AP disappears (device switched to STA)"
    echo ""

    local elapsed=0
    local interval=5
    while [ "$elapsed" -lt "$PROVISIONING_WAIT_TIMEOUT" ]; do
        sleep "$interval"
        elapsed=$((elapsed + interval))

        # If status says connected, provisioning is definitely done
        local current_status=""
        current_status=$(curl -s --max-time 3 "http://$ESP_IP/status" 2>/dev/null || true)
        if echo "$current_status" | grep -q "\"status\":\"connected\""; then
            print_success "Provisioning accepted by device (/status=connected)"
            echo "$current_status" | python3 -m json.tool 2>/dev/null || true
            return 0
        fi

        # If AP is gone, this usually means credentials were submitted and device switched network
        if ! ping -c 1 -W 1 "$ESP_IP" >/dev/null 2>&1; then
            print_success "Provisioning likely completed (AP no longer reachable; device switched to STA)"
            return 0
        fi

        if [ $((elapsed % 30)) -eq 0 ]; then
            print_info "Still waiting... (${elapsed}s elapsed)"
        fi
    done

    print_warning "Timed out waiting for provisioning transition."
    print_info "If you already submitted in portal, continue and verify via serial monitor."
    return 1
}

monitor_serial() {
    print_step "Starting serial monitor (shows MQTT topic data)"
    print_info "Press Ctrl+C to stop monitoring"
    print_info "Port: $ESP_PORT"
    print_info ""
    idf.py -p "$ESP_PORT" monitor
}

# Main execution
main() {
    echo "=========================================="
    echo "ESP32 WiFi Provisioning - Run Script"
    echo "=========================================="
    echo ""
    
    # Step 0: Cleanup first (before any operations)
    cleanup_serial_port
    echo ""
    
    setup_esp_idf
    build_project
    flash_esp32
    wait_for_boot
    
    local ap_result=0
    connect_to_ap || ap_result=$?

    if [ "$ap_result" -eq 0 ]; then
        # Wait for HTTP POST provisioning (no CLI input)
        if wait_for_http_provisioning; then
            echo ""
            print_success "Provisioning completed!"
        else
            echo ""
            print_warning "Provisioning may still be in progress"
            print_info "Send HTTP POST to http://$ESP_IP/provision when ready"
        fi
        
        echo ""
        print_info "Waiting for device to connect to WiFi and verify internet..."
        print_info "This may take 10-15 seconds..."
        sleep 15
        
        echo ""
        print_info "========================================"
        print_info "Verifying Internet Connectivity"
        print_info "========================================"
        print_info "Checking if ESP32 can access internet..."
        print_info "Endpoint: https://mqtt-test-puf8.onrender.com/api/"
        print_info ""
        print_info "Check serial monitor for detailed logs:"
        print_info "  idf.py -p $ESP_PORT monitor"
        print_info ""
        print_info "You should see in serial monitor:"
        print_info "  - 'Internet Connectivity Verification'"
        print_info "  - 'HTTP Status Code: 200'"
        print_info "  - 'INTERNET CONNECTIVITY VERIFIED!'"
        print_info "  - 'Provisioning flow 100% complete!'"
        print_info "  - Full API response from endpoint"
        print_info ""
        print_info "========================================"
        print_info "Development Mode - Handing off to monitor"
        print_info "========================================"
        echo ""
        monitor_serial
    elif [ "$ap_result" -eq 2 ]; then
        echo ""
        print_info "AP not found. Assuming credential-reuse boot path (STA mode)."
        print_info "Handing off to serial monitor to observe:"
        print_info "  - WiFi STA connect"
        print_info "  - CSR submission"
        print_info "  - Certificate download/save"
        print_info "  - MQTT mTLS connect"
        echo ""
        monitor_serial
    else
        print_error "Could not connect to ESP32 AP"
        print_info "Check serial monitor for errors:"
        print_info "  idf.py -p $ESP_PORT monitor"
        exit 1
    fi
}

# Run main function
main "$@"



