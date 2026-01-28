# StatsClient - Arduino Version

This is the Arduino/PlatformIO version of the ESP32 WiFi Provisioning with mTLS MQTT application.

## Project Structure

```
arduino/
├── statsclient.ino          # Main Arduino sketch
├── WiFiProvisioning.h       # WiFi provisioning header
├── WiFiProvisioning.cpp     # WiFi provisioning implementation
├── CertificateManager.h     # Certificate manager header
├── CertificateManager.cpp   # Certificate manager implementation
├── InternetVerification.h   # Internet verification header
├── InternetVerification.cpp # Internet verification implementation
├── MQTTHandler.h            # MQTT handler header
├── MQTTHandler.cpp          # MQTT handler implementation
├── DeviceKeys.h             # Device keys and CSR
├── platformio.ini           # PlatformIO configuration
└── README.md                # This file
```

## Key Differences from ESP-IDF Version

### 1. **Framework**
- **ESP-IDF**: Uses FreeRTOS directly, esp_http_server, esp_wifi
- **Arduino**: Uses Arduino framework, ESPAsyncWebServer, WiFi library

### 2. **Storage**
- **ESP-IDF**: Uses NVS (Non-Volatile Storage) API
- **Arduino**: Uses Preferences library (wrapper around NVS)

### 3. **HTTP Server**
- **ESP-IDF**: `esp_http_server` with synchronous handlers
- **Arduino**: `ESPAsyncWebServer` with asynchronous handlers

### 4. **Logging**
- **ESP-IDF**: `esp_log.h` with ESP_LOGI, ESP_LOGE, etc.
- **Arduino**: `Serial` for logging

### 5. **JSON Parsing**
- **ESP-IDF**: cJSON library
- **Arduino**: ArduinoJson library

### 6. **MQTT**
- **ESP-IDF**: `mqtt_client.h` (ESP-IDF MQTT client)
- **Arduino**: `PubSubClient` library

### 7. **HTTP Client**
- **ESP-IDF**: `esp_http_client.h`
- **Arduino**: `HTTPClient` + `WiFiClientSecure`

### 8. **Timing**
- **ESP-IDF**: `esp_timer_get_time()` for microseconds
- **Arduino**: `millis()` / `micros()` for milliseconds/microseconds

## Dependencies

Install via PlatformIO or Arduino Library Manager:

- **ESPAsyncWebServer** (v3.0.0+)
- **AsyncTCP** (v1.1.1+) - Required by ESPAsyncWebServer
- **ArduinoJson** (v6.21.3+)
- **PubSubClient** (v2.8.0+)
- **WiFi** (built-in)
- **Preferences** (built-in)
- **HTTPClient** (built-in)
- **WiFiClientSecure** (built-in)

## Configuration

Update these values in `statsclient.ino`:

```cpp
#define AP_SSID_PREFIX "ESP32-Prov"
#define AP_PASSWORD "prov12345678"
#define BACKEND_URL "https://your-backend-url.com"
#define MQTT_BROKER "your-mqtt-broker.com"
#define MQTT_PORT 8883
```

## Setup and Installation

### Prerequisites

- **Python 3** (3.6 or higher)
- **pip** (Python package manager)
- **USB cable** to connect ESP32 board
- **ESP32 board** (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)

### Manual Setup (recommended)

On your machine right now, **Python exists but pip is not installed** (this is common on Ubuntu/Debian system Python).

#### 1. Install pip

```bash
sudo apt update && sudo apt install -y python3-pip
```

Verify:

```bash
python3 -m pip --version
```

#### 2. Install PlatformIO (user install)

**Important (Ubuntu 24.04 / Noble + Python 3.12):**

- **Don’t use** `sudo apt install platformio` — Ubuntu’s `platformio` package can be **too old** (e.g. `platformio==4.3.4`) and can **crash on Python 3.12 / click 8** with:
  `AttributeError: 'PlatformioCLI' object has no attribute 'resultcallback'`.
- **Also** `python3 -m pip install --user ...` can fail with:
  `error: externally-managed-environment` (PEP 668).

✅ Use **pipx** (recommended) or a **venv**.

##### Option A (recommended): pipx

```bash
sudo apt update
sudo apt install -y pipx
pipx ensurepath
```

Open a new terminal (or `source ~/.bashrc`), then:

```bash
pipx install platformio
pio --version
```

##### Option B: venv (no pipx)

```bash
sudo apt update
sudo apt install -y python3-venv

python3 -m venv ~/.venvs/platformio
~/.venvs/platformio/bin/pip install -U platformio
~/.venvs/platformio/bin/pio --version
```

#### 3. Install project libraries (manual)

PlatformIO will install dependencies declared in `platformio.ini`:

```bash
cd arduino
pio pkg install
```

If you want to install the key libraries explicitly:

```bash
pio lib install "bblanchon/ArduinoJson@^6.21.3"
pio lib install https://github.com/me-no-dev/ESPAsyncWebServer.git
pio lib install https://github.com/me-no-dev/AsyncTCP.git
pio lib install "knolleary/PubSubClient@^2.8.0"
```

### Setup script (optional)

If you still want a helper, `setup.sh` only does:
- verify `python3 -m pip` works
- install PlatformIO with `python3 -m pip --user`
- run `pio pkg install`

```bash
cd arduino
chmod +x setup.sh
./setup.sh
```

## Building and Flashing

### Using PlatformIO (Command Line)

**Build the project:**
```bash
cd arduino
pio run
```

**Upload to ESP32:**
```bash
pio run -t upload
```

**Monitor serial output:**
```bash
pio device monitor
```

**Or use the build script:**
```bash
chmod +x build.sh
./build.sh
```

### Using PlatformIO IDE (VS Code)

1. Open VS Code
2. Open the `arduino` folder
3. PlatformIO will automatically detect the project
4. Click the **PlatformIO** icon in the sidebar
5. Use the **Build**, **Upload**, and **Monitor** buttons

### Using Arduino IDE

1. **Install ESP32 Board Support:**
   - Open Arduino IDE
   - Go to `File` → `Preferences`
   - Add to "Additional Board Manager URLs":
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Go to `Tools` → `Board` → `Boards Manager`
   - Search for "ESP32" and install "esp32 by Espressif Systems"

2. **Install Required Libraries:**
   - Go to `Tools` → `Manage Libraries`
   - Install each library:
     - **ArduinoJson** (by Benoit Blanchon) - version 6.21.3 or higher
     - **ESPAsyncWebServer** (by me-no-dev) - version 3.0.0 or higher
     - **AsyncTCP** (by me-no-dev) - version 1.1.1 or higher
     - **PubSubClient** (by Nick O'Leary) - version 2.8.0 or higher

3. **Open and Upload:**
   - Open `statsclient.ino` in Arduino IDE
   - Select your board: `Tools` → `Board` → `ESP32 Arduino` → Your ESP32 model
   - Select the port: `Tools` → `Port` → Your USB port
   - Click **Upload** button

### Troubleshooting

**PlatformIO not found:**
```bash
# Add PlatformIO to PATH (Linux/macOS)
export PATH=$PATH:~/.platformio/penv/bin

# Or reinstall PlatformIO
pip3 install --user platformio
```

**Permission denied on USB port (Linux):**
```bash
sudo usermod -a -G dialout $USER
# Then logout and login again
```

**Library installation fails:**
```bash
# Clear PlatformIO cache and reinstall
pio pkg update
pio pkg install
```

## API Endpoints

Same as ESP-IDF version:

- `GET /` - Root endpoint (returns available endpoints)
- `GET /local-wifi` - Scan WiFi networks
- `POST /provision` - Submit WiFi credentials
- `GET /status` - Get provisioning status
- `OPTIONS /*` - CORS preflight support

## CORS Support

The Arduino version includes the same CORS support:
- Allows `localhost:3000` (development)
- Allows `statsnapp.vercel.app` (production)
- Wildcard fallback for other origins

## Logging

All HTTP requests and responses are logged via Serial:
- Request method, URI, headers
- Response status, body
- Processing time

## Notes

- The Arduino version uses the same state machine logic as ESP-IDF
- All functionality is preserved
- CORS headers work the same way
- Middleware logging is included
- Certificate management works identically
