# Running All Setup Scripts

## Current Status

Your system needs:
- ✅ Python 3.12.3 (installed)
- ❌ pip3 (needs installation with sudo)
- ❌ PlatformIO (will install after pip3)

## Quick Command to Run Everything

**Option 1: Automated (requires sudo password once)**

```bash
cd /home/statsnapp/Desktop/statsclient/arduino
./run_all_setup.sh
```

This will:
1. Install pip3 (requires sudo)
2. Install PlatformIO
3. Install all project dependencies
4. Verify installation

**Option 2: Manual Step-by-Step**

```bash
cd /home/statsnapp/Desktop/statsclient/arduino

# Step 1: Install pip3 (requires sudo)
sudo apt update
sudo apt install -y python3-pip

# Step 2: Install PlatformIO
pip3 install --user platformio

# Step 3: Add to PATH
export PATH=$PATH:~/.local/bin
export PATH=$PATH:~/.platformio/penv/bin

# Step 4: Verify
pio --version

# Step 5: Install dependencies
./install_dependencies.sh

# Step 6: Build (optional)
./build.sh
```

## Script Execution Order

The scripts should be run in this order:

1. **run_all_setup.sh** (or manually install pip3 first)
   - Installs pip3
   - Installs PlatformIO
   - Installs dependencies

2. **install_dependencies.sh**
   - Installs Arduino libraries
   - Only works after PlatformIO is installed

3. **build.sh**
   - Builds the project
   - Uploads to ESP32
   - Opens serial monitor

## What Each Script Does

### setup.sh
- Checks Python 3
- Installs pip3 (interactive)
- Installs PlatformIO
- Installs dependencies

### install_dependencies.sh
- Installs ArduinoJson
- Installs ESPAsyncWebServer
- Installs AsyncTCP
- Installs PubSubClient

### build.sh
- Builds the project
- Prompts to upload
- Opens serial monitor

### run_all_setup.sh (NEW)
- Does everything automatically
- Handles pip3 installation
- Non-interactive where possible

## Troubleshooting

**If scripts fail:**
1. Make sure pip3 is installed: `pip3 --version`
2. Make sure PlatformIO is installed: `pio --version`
3. Check PATH: `echo $PATH | grep platformio`

**If permission denied:**
```bash
chmod +x *.sh
```

**If pio command not found:**
```bash
export PATH=$PATH:~/.local/bin
export PATH=$PATH:~/.platformio/penv/bin
```
