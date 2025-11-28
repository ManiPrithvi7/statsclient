#!/bin/bash
# Quick test script to see HTTP request logging
# Usage: ./test_logging.sh

ESP_PORT="/dev/ttyACM0"
ESP_IP="192.168.4.1"

echo "=========================================="
echo "ESP32 HTTP Request Logging Test"
echo "=========================================="
echo ""
echo "This script will:"
echo "1. Start serial monitor in background"
echo "2. Wait 5 seconds for device to be ready"
echo "3. Make a curl request to /local-wifi"
echo "4. Show the logs captured"
echo ""
echo "Make sure:"
echo "- ESP32 is connected at $ESP_PORT"
echo "- You're connected to ESP32-Prov WiFi"
echo ""
read -p "Press Enter to continue..."

# Start monitor in background, capture output
echo "Starting serial monitor..."
cd "$(dirname "$0")"
source esp-idf/export.sh >/dev/null 2>&1

# Create a named pipe for monitor output
PIPE=$(mktemp -u)
mkfifo "$PIPE"

# Start monitor and redirect to pipe (with timeout)
timeout 15 idf.py -p "$ESP_PORT" monitor 2>&1 | tee "$PIPE" > /tmp/monitor_output.txt &
MONITOR_PID=$!

# Wait for monitor to start
sleep 3

echo "Making HTTP request to $ESP_IP/local-wifi..."
curl -s "http://$ESP_IP/local-wifi" > /dev/null

# Wait a bit for logs to appear
sleep 3

# Kill monitor
kill $MONITOR_PID 2>/dev/null
wait $MONITOR_PID 2>/dev/null

# Clean up pipe
rm -f "$PIPE"

echo ""
echo "=========================================="
echo "Captured Logs:"
echo "=========================================="
grep -E "(SCAN_HANDLER|PROVISION_HANDLER|STATUS_HANDLER|INCOMING|OUTGOING|>>>|<<<|REQUEST|RESPONSE|wifi_prov.*HTTP)" /tmp/monitor_output.txt 2>/dev/null | head -50 || echo "No logs found. Make sure:"
echo ""
echo "If no logs appear:"
echo "1. Device might not be running the new firmware"
echo "2. Run: ./run.sh (to rebuild and flash)"
echo "3. Then manually run: idf.py -p $ESP_PORT monitor"
echo "4. In another terminal, make the curl request"

