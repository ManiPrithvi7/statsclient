#!/bin/bash
# Build and upload script for StatsClient Arduino project

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "StatsClient - Building Project"
echo "========================================"
echo ""

# Find pio (PlatformIO) even if PATH isn't configured
PIO="$HOME/.local/bin/pio"
if [ ! -x "$PIO" ]; then
    PIO="$HOME/.platformio/penv/bin/pio"
fi
if [ ! -x "$PIO" ] && command -v pio &> /dev/null; then
    PIO="$(command -v pio)"
fi

if [ ! -x "$PIO" ]; then
    echo "ERROR: PlatformIO is not installed (or 'pio' not found)."
    echo ""
    echo "Manual install:"
    echo "  sudo apt update && sudo apt install -y python3-pip"
    echo "  python3 -m pip install --user --upgrade platformio"
    echo "  export PATH=\$PATH:\$HOME/.local/bin"
    echo ""
    echo "Or run:"
    echo "  ./setup.sh"
    exit 1
fi

# Build the project
echo "Building project..."
$PIO run

echo ""
echo "========================================"
echo "Build successful!"
echo "========================================"
echo ""

# Ask if user wants to upload
read -p "Upload to ESP32? (y/n): " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Uploading..."
    $PIO run -t upload
    
    echo ""
    echo "========================================"
    echo "Upload complete!"
    echo "========================================"
    echo ""
    
    # Ask if user wants to monitor
    read -p "Open serial monitor? (y/n): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Opening serial monitor (Ctrl+C to exit)..."
        $PIO device monitor
    fi
fi
