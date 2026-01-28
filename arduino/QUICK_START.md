# Quick Start Guide

## Current System Status

Based on your system:
- ✅ Python 3.12.3 is installed
- ❌ pip3 is not installed
- ❌ PlatformIO is not installed

## Installation Steps

### Step 1: Install pip3

```bash
sudo apt install python3-pip
```

### Step 2: Install PlatformIO

```bash
pip3 install --user platformio
```

### Step 3: Add PlatformIO to PATH

```bash
# Add to your ~/.bashrc
echo 'export PATH=$PATH:~/.local/bin' >> ~/.bashrc
source ~/.bashrc

# Or for this session only:
export PATH=$PATH:~/.local/bin
```

### Step 4: Verify Installation

```bash
pio --version
```

### Step 5: Install Project Dependencies

```bash
cd /home/statsnapp/Desktop/statsclient/arduino
pio pkg install
```

### Step 6: Build and Upload

```bash
# Build
pio run

# Upload (connect ESP32 first)
pio run -t upload

# Monitor serial output
pio device monitor
```

## Or Use the Automated Script

After installing pip3, you can run:

```bash
cd /home/statsnapp/Desktop/statsclient/arduino
chmod +x setup.sh
./setup.sh
```

This will install everything automatically.

## Troubleshooting

**If pio command not found after installation:**
```bash
# Find PlatformIO installation
find ~ -name pio 2>/dev/null

# Add to PATH (replace with actual path if different)
export PATH=$PATH:~/.platformio/penv/bin
```

**If USB port permission denied:**
```bash
sudo usermod -a -G dialout $USER
# Logout and login again
```
