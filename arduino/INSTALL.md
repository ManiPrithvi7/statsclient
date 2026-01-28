# Installation Guide - StatsClient Arduino Project

This guide will help you set up your development environment to build and upload the StatsClient Arduino project.

## Quick Start

Run the automated setup script:

```bash
cd arduino
./setup.sh
```

This will install everything you need automatically.

## Manual Installation

### Step 1: Install Python 3 and pip

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install python3 python3-pip python3-venv
```

**macOS:**
```bash
# Using Homebrew
brew install python3

# Or download from python.org
```

**Windows:**
- Download Python 3 from https://www.python.org/downloads/
- Make sure to check "Add Python to PATH" during installation

**Verify installation:**
```bash
python3 --version  # Should show Python 3.6 or higher
pip3 --version     # Should show pip version
```

### Step 2: Install PlatformIO Core

**Linux/macOS:**
```bash
pip3 install --user platformio
```

**Windows:**
```bash
pip install --user platformio
```

**Add to PATH (if needed):**
```bash
# Linux/macOS - Add to ~/.bashrc or ~/.zshrc
export PATH=$PATH:~/.local/bin

# Or for PlatformIO specifically:
export PATH=$PATH:~/.platformio/penv/bin
```

**Verify installation:**
```bash
pio --version
```

### Step 3: Install Project Dependencies

Navigate to the project directory:

```bash
cd /home/statsnapp/Desktop/statsclient/arduino
```

Install dependencies:

```bash
# Option 1: Use the install script
./install_dependencies.sh

# Option 2: Use PlatformIO directly
pio pkg install
```

This will install:
- **ArduinoJson** (v6.21.3+) - JSON parsing
- **ESPAsyncWebServer** (v3.0.0+) - Async HTTP server
- **AsyncTCP** (v1.1.1+) - Async TCP library
- **PubSubClient** (v2.8.0+) - MQTT client

### Step 4: Verify Installation

Check that everything is installed:

```bash
# Check PlatformIO
pio --version

# Check installed packages
pio pkg list

# Check ESP32 platform
pio platform show espressif32
```

## Building the Project

### Option 1: Using the Build Script

```bash
cd arduino
./build.sh
```

This script will:
1. Build the project
2. Ask if you want to upload
3. Ask if you want to open serial monitor

### Option 2: Using PlatformIO Commands

**Build only:**
```bash
cd arduino
pio run
```

**Build and upload:**
```bash
pio run -t upload
```

**Monitor serial output:**
```bash
pio device monitor
```

**Clean build:**
```bash
pio run -t clean
```

### Option 3: Using PlatformIO IDE (VS Code)

1. Install [VS Code](https://code.visualstudio.com/)
2. Install [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
3. Open the `arduino` folder in VS Code
4. PlatformIO will automatically detect the project
5. Use the PlatformIO toolbar buttons:
   - **✓** Build
   - **→** Upload
   - **🔌** Monitor

## Troubleshooting

### PlatformIO Command Not Found

**Linux/macOS:**
```bash
# Add to PATH
export PATH=$PATH:~/.local/bin
export PATH=$PATH:~/.platformio/penv/bin

# Or reinstall with user flag
pip3 install --user --upgrade platformio
```

**Windows:**
- Restart terminal/command prompt after installation
- Or add `%USERPROFILE%\.platformio\penv\Scripts` to PATH

### Permission Denied on USB Port (Linux)

```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER

# Logout and login again, or:
newgrp dialout
```

### Library Installation Fails

```bash
# Clear cache and reinstall
pio pkg update
pio pkg install

# Or install libraries individually
pio lib install "bblanchon/ArduinoJson@^6.21.3"
pio lib install "links2004/ESPAsyncWebServer@^3.0.0"
pio lib install "links2004/AsyncTCP@^1.1.1"
pio lib install "knolleary/PubSubClient@^2.8.0"
```

### Build Errors

**Clean and rebuild:**
```bash
pio run -t clean
pio run
```

**Check for missing dependencies:**
```bash
pio pkg list
```

**Update PlatformIO:**
```bash
pip3 install --user --upgrade platformio
pio platform update espressif32
```

### Upload Errors

**Check USB connection:**
```bash
# List available ports
pio device list

# Or on Linux:
ls /dev/ttyUSB* /dev/ttyACM*

# Or on macOS:
ls /dev/cu.*
```

**Specify port manually:**
```bash
pio run -t upload --upload-port /dev/ttyUSB0
```

**Check upload speed in platformio.ini:**
- Default is 921600 baud
- Try lower speeds if upload fails: 115200, 460800

## Project Structure

```
arduino/
├── statsclient.ino              # Main Arduino sketch
├── WiFiProvisioning.h/cpp       # WiFi provisioning module
├── CertificateManager.h/cpp     # Certificate management
├── InternetVerification.h/cpp   # Internet connectivity test
├── MQTTHandler.h/cpp            # MQTT client with mTLS
├── DeviceKeys.h                  # Device keys and CSR
├── platformio.ini               # PlatformIO configuration
├── setup.sh                      # Automated setup script
├── build.sh                      # Build and upload script
├── install_dependencies.sh       # Dependency installer
├── README.md                     # Project documentation
└── INSTALL.md                    # This file
```

## Next Steps

After installation:

1. **Configure the project:**
   - Edit `statsclient.ino` and update:
     - `BACKEND_URL` - Your backend server URL
     - `MQTT_BROKER` - Your MQTT broker hostname
     - `MQTT_PORT` - MQTT broker port (usually 8883 for mTLS)

2. **Connect your ESP32:**
   - Connect ESP32 board via USB
   - Note the port name (e.g., `/dev/ttyUSB0` or `COM3`)

3. **Build and upload:**
   ```bash
   ./build.sh
   ```

4. **Monitor output:**
   - Serial monitor will show provisioning status
   - Look for "WiFi provisioning started" message
   - Connect to AP: `ESP32-Prov-XXXX`
   - Password: `prov12345678`

## Additional Resources

- [PlatformIO Documentation](https://docs.platformio.org/)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- [ArduinoJson](https://arduinojson.org/)
