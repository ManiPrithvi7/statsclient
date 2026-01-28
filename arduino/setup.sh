#!/bin/bash
# Setup script for StatsClient Arduino/PlatformIO project
# This script installs PlatformIO and the project dependencies (no sudo, no prompts).

set -e

echo "========================================"
echo "StatsClient Arduino Setup Script"
echo "========================================"
echo ""

# Check if Python 3 is installed
if ! command -v python3 &> /dev/null; then
    echo "ERROR: Python 3 is required but not installed."
    echo "Please install Python 3 first:"
    echo "  Ubuntu/Debian: sudo apt-get install python3 python3-pip"
    echo "  macOS: brew install python3"
    echo "  Or download from: https://www.python.org/downloads/"
    exit 1
fi

echo "✓ Python 3 found: $(python3 --version)"
echo ""

# Check if pip is available (Ubuntu/Debian system python often doesn't ship it by default)
if ! python3 -m pip --version &> /dev/null; then
    echo "ERROR: Python pip is not installed for this Python."
    echo ""
    echo "Install it (Ubuntu/Debian):"
    echo "  sudo apt update && sudo apt install -y python3-pip"
    echo ""
    echo "Then re-run:"
    echo "  ./setup.sh"
    exit 1
fi

echo "✓ pip found: $(python3 -m pip --version)"
echo ""

echo "Installing PlatformIO..."
echo ""
echo "Ubuntu 24.04+ may block pip installs with: externally-managed-environment (PEP 668)."
echo "Recommended install method: pipx."
echo ""
echo "Run:"
echo "  sudo apt update && sudo apt install -y pipx"
echo "  pipx ensurepath"
echo "  pipx install platformio"
echo ""
echo "Then open a new terminal and re-run:"
echo "  ./setup.sh"
echo ""

if command -v pio &> /dev/null; then
    PIO="$(command -v pio)"
elif [ -x "$HOME/.local/bin/pio" ]; then
    PIO="$HOME/.local/bin/pio"
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
    PIO="$HOME/.platformio/penv/bin/pio"
else
    echo "ERROR: 'pio' not found. Install PlatformIO using pipx (recommended) or venv."
    exit 1
fi

echo ""
if [ -x "$PIO" ]; then
    echo "✓ PlatformIO installed: $($PIO --version)"
else
    echo "ERROR: PlatformIO installed but 'pio' is not on PATH."
    echo ""
    echo "NOTE: If you installed PlatformIO via apt (`/usr/bin/pio`) on Ubuntu,"
    echo "it may be an old version and can crash on Python 3.12."
    echo "Recommended: remove apt PlatformIO and use pip user install instead:"
    echo "  sudo apt remove -y platformio"
    echo "  sudo apt install -y pipx"
    echo "  pipx install platformio"
    echo ""
    echo "For this terminal session run:"
    echo "  export PATH=\$PATH:\$HOME/.local/bin"
    echo ""
    echo "Or add to ~/.bashrc:"
    echo "  echo 'export PATH=\$PATH:\$HOME/.local/bin' >> ~/.bashrc"
    exit 1
fi
echo ""

# Navigate to project directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Project directory: $SCRIPT_DIR"
echo ""

# Install project dependencies
echo "Installing project dependencies..."
echo "This may take a few minutes on first run..."
echo ""

$PIO pkg install

echo ""
echo "========================================"
echo "✓ Setup Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Connect your ESP32 board via USB"
echo "  2. Run: pio run -t upload"
echo "  3. Monitor serial output: pio device monitor"
echo ""
echo "Or use the build script: ./build.sh"
echo ""
