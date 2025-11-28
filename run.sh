#!/bin/bash
# ESP32 WiFi Provisioning - Build, Flash, and Test Script
# This single script handles everything: build → flash → connect → test

set -e

# Configuration
ESP_PORT="/dev/ttyACM0"
AP_SSID="ESP32-Prov"
AP_PASSWORD="prov12345678"
ESP_IP="192.168.4.1"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
        source "$PROJECT_DIR/esp-idf/export.sh" >/dev/null 2>&1
        print_success "ESP-IDF environment loaded"
    else
        print_error "ESP-IDF not found. Please install ESP-IDF first."
        exit 1
    fi
}

# Step 2: Build project
build_project() {
    print_step "Building project"
    cd "$PROJECT_DIR"
    if idf.py build >/dev/null 2>&1; then
        print_success "Build completed"
    else
        print_error "Build failed. Check errors above."
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
    FLASH_OUTPUT=$(mktemp)
    if idf.py -p "$ESP_PORT" flash > "$FLASH_OUTPUT" 2>&1; then
        # Check for success indicators
        if grep -q "Hash of data verified" "$FLASH_OUTPUT" || grep -q "Leaving" "$FLASH_OUTPUT"; then
            print_success "Firmware flashed successfully"
            rm -f "$FLASH_OUTPUT"
        else
            print_error "Flash may have failed - no success indicator found"
            print_info "Flash output:"
            tail -15 "$FLASH_OUTPUT"
            rm -f "$FLASH_OUTPUT"
            exit 1
        fi
    else
        print_error "Flash failed"
        print_info "Flash output:"
        tail -20 "$FLASH_OUTPUT"
        rm -f "$FLASH_OUTPUT"
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
        print_error "ESP32 AP '$AP_SSID' not found after $MAX_RETRIES attempts"
        print_info "Possible reasons:"
        print_info "  1. Device needs more time to boot (try waiting 10-15 more seconds)"
        print_info "  2. Device encountered an error during boot"
        print_info "  3. WiFi initialization failed"
        print_info ""
        print_info "Check serial monitor for details:"
        print_info "  idf.py -p $ESP_PORT monitor"
        return 1
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

# Step 6: Interactive WiFi provisioning
wait_for_http_provisioning() {
    print_step "WiFi Provisioning via HTTP POST"
    
    # Verify ESP32 is ready
    print_info "Verifying ESP32 is ready..."
    if STATUS=$(curl -s --max-time 5 "http://$ESP_IP/status" 2>/dev/null); then
        if echo "$STATUS" | grep -q "status"; then
            print_success "ESP32 is ready and waiting for HTTP POST /provision"
        else
            print_error "ESP32 not responding correctly"
            return 1
        fi
    else
        print_error "Cannot connect to ESP32"
        return 1
    fi
    
    echo ""
    print_info "Scanning for available WiFi networks (optional reference)..."
    print_info "This may take 15-20 seconds, please wait..."
    
    # Scan for WiFi networks (optional, for user reference)
    WIFI_SCAN=$(timeout 30 curl -s --max-time 30 "http://$ESP_IP/local-wifi" 2>/dev/null)
    
    if [ $? -eq 0 ] && echo "$WIFI_SCAN" | grep -q "networks"; then
        echo ""
        print_success "Available WiFi Networks:"
        echo ""
        
        # Display networks using Python
        python3 << PYEOF
import json
import sys

try:
    wifi_scan = '''$WIFI_SCAN'''
    data = json.loads(wifi_scan)
    networks = data.get('networks', [])
    
    if not networks:
        print("  No networks found")
    else:
        print(f"  {'#':<4} {'SSID':<30} {'RSSI':<8} {'Channel':<8} {'Security':<10}")
        print("  " + "-" * 70)
        for i, net in enumerate(networks, 1):
            ssid = net.get('ssid', 'Unknown')
            rssi = net.get('rssi', 0)
            channel = net.get('channel', 0)
            secure = "Yes" if net.get('secure', False) else "No"
            print(f"  {i:<4} {ssid:<30} {rssi:<8} {channel:<8} {secure:<10}")
except Exception as e:
    print(f"  Error parsing networks: {e}")
PYEOF
    else
        print_warning "Could not scan networks (optional - you can still provision)"
    fi
    
    echo ""
    echo ""
    print_info "========================================"
    print_info "Send Credentials via HTTP POST"
    print_info "========================================"
    echo ""
    print_info "The device is waiting for credentials via HTTP POST request."
    print_info "Send a POST request to: http://$ESP_IP/provision"
    echo ""
    print_info "Example using curl:"
    echo ""
    echo "  curl -X POST http://$ESP_IP/provision \\"
    echo "    -H \"Content-Type: application/json\" \\"
    echo "    -H \"Authorization: Bearer YOUR_BEARER_TOKEN\" \\"
    echo "    -d '{"
    echo "      \"ssid\": \"YourWiFiSSID\","
    echo "      \"password\": \"YourWiFiPassword\","
    echo "      \"device_id\": \"device_0070\","
    echo "      \"provisioning_token\": \"your_prov_token\""
    echo "    }'"
    echo ""
    print_info "Or use any HTTP client (Postman, browser extension, etc.)"
    echo ""
    print_info "Required JSON payload:"
    echo "  {"
    echo "    \"ssid\": \"WiFi network name\","
    echo "    \"password\": \"WiFi password (empty string for open networks)\","
    echo "    \"device_id\": \"device identifier\","
    echo "    \"provisioning_token\": \"provisioning token\""
    echo "  }"
    echo ""
    print_info "Required HTTP header:"
    echo "  Authorization: Bearer YOUR_BEARER_TOKEN"
    echo ""
    print_info "Waiting for HTTP POST /provision request..."
    print_info "The device will automatically process the request when received."
    echo ""
    
    # State variable: true if credentials are present and device is connected
    CREDENTIALS_PRESENT=false
    
    # Check initial state
    print_info "Checking initial device provisioning status..."
    INITIAL_STATUS=$(curl -s --max-time 3 "http://$ESP_IP/status" 2>/dev/null)
    
    if echo "$INITIAL_STATUS" | grep -q "\"status\":\"connected\""; then
        CREDENTIALS_PRESENT=true
        print_success "Device is already connected to WiFi!"
        echo ""
        print_info "Device Status Response (via HTTP):"
        echo "$INITIAL_STATUS" | python3 -m json.tool 2>/dev/null || echo "$INITIAL_STATUS"
        return 0
    else
        CREDENTIALS_PRESENT=false
        print_info "Device is in provisioning mode - waiting for credentials"
    fi
    
    echo ""
    print_info "Waiting for state change (checking every 10 seconds)..."
    print_info "Press Ctrl+C to exit"
    echo ""
    
    # Wait for state change - check less frequently to avoid CPU burn
    local check_interval=0
    while [ "$CREDENTIALS_PRESENT" = false ]; do
        # Wait 10 seconds between checks (less frequent = less CPU usage)
        sleep 10
        check_interval=$((check_interval + 1))
        
        # Check if state has changed
        CURRENT_STATUS=$(curl -s --max-time 3 "http://$ESP_IP/status" 2>/dev/null)
        
        if echo "$CURRENT_STATUS" | grep -q "\"status\":\"connected\""; then
            # State changed! Credentials are now present
            CREDENTIALS_PRESENT=true
            
            echo ""
            print_success "========================================"
            print_success "State Changed - Credentials Present!"
            print_success "========================================"
            echo ""
            print_info "Fetching device status via HTTP call..."
            
            # Make HTTP call to get full status and print response
            FINAL_STATUS=$(curl -s --max-time 5 "http://$ESP_IP/status" 2>/dev/null)
            
            if [ -n "$FINAL_STATUS" ]; then
                print_success "Device Status Response (via HTTP):"
                echo ""
                # Pretty print JSON if possible
                echo "$FINAL_STATUS" | python3 -m json.tool 2>/dev/null || echo "$FINAL_STATUS"
                echo ""
                
                # Extract and display key information
                if echo "$FINAL_STATUS" | grep -q "\"ip\""; then
                    DEVICE_IP=$(echo "$FINAL_STATUS" | python3 -c "import sys, json; d=json.load(sys.stdin); print(d.get('ip', 'N/A'))" 2>/dev/null)
                    if [ -n "$DEVICE_IP" ] && [ "$DEVICE_IP" != "N/A" ]; then
                        print_success "Device IP Address: $DEVICE_IP"
                    fi
                fi
            else
                print_warning "Could not fetch status via HTTP, but device appears connected"
            fi
            
            print_success "Provisioning successful!"
            return 0
        fi
        
        # Show progress indicator every 6 checks (60 seconds) - minimal CPU usage
        if [ $((check_interval % 6)) -eq 0 ]; then
            print_info "Still waiting... ($((check_interval * 10)) seconds elapsed)"
            print_info "Send HTTP POST to http://$ESP_IP/provision when ready"
        fi
    done
    
    return 0
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
    
    if connect_to_ap; then
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
        print_info "Development Mode - Script Running"
        print_info "========================================"
        print_info "Press Ctrl+C to stop"
        print_info ""
        print_info "Monitor serial output for internet verification:"
        print_info "  idf.py -p $ESP_PORT monitor"
        print_info ""
        
        # Keep script running until Ctrl+C
        trap 'echo ""; print_info "Stopping..."; exit 0' INT TERM
        
        # Wait indefinitely (until Ctrl+C)
        while true; do
            sleep 1
        done
    else
        print_error "Could not connect to ESP32 AP"
        print_info "Check serial monitor for errors:"
        print_info "  idf.py -p $ESP_PORT monitor"
        exit 1
    fi
}

# Run main function
main "$@"



