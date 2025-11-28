# WiFi Provisioning with mTLS MQTT - ESP-IDF Project

This project implements a complete device provisioning flow for ESP32 devices:

1. **WiFi Provisioning** - Device starts as Access Point, receives WiFi credentials via HTTP
2. **CSR Submission** - Device submits Certificate Signing Request to backend
3. **Certificate Storage** - Device receives and stores signed certificates
4. **mTLS MQTT Connection** - Device connects to MQTT broker using mutual TLS authentication

## Project Structure

```
esp/
├── CMakeLists.txt              # Root project configuration
├── main/
│   ├── CMakeLists.txt          # Component build configuration
│   ├── idf_component.yml       # Component dependencies (cJSON)
│   ├── Kconfig.projbuild       # Configuration options
│   ├── main.c                  # Main state machine
│   ├── wifi_provisioning.c/h   # WiFi AP + HTTP server
│   ├── certificate_manager.c/h  # CSR submission & cert storage
│   ├── mqtt_client.c/h         # mTLS MQTT client
│   └── device_keys.h            # Private key & CSR (generated)
└── README.md                   # This file
```

## Features

- ✅ WiFi Access Point mode for provisioning
- ✅ HTTP server with RESTful endpoints:
  - `GET /scan` - Scan for available WiFi networks
  - `POST /provision` - Submit WiFi credentials and provisioning token
  - `GET /status` - Get provisioning status
- ✅ Automatic WiFi connection after provisioning
- ✅ CSR submission to backend via HTTPS
- ✅ Certificate storage in NVS
- ✅ mTLS MQTT connection to broker
- ✅ State machine for orchestration

## Quick Start

**One command to build, flash, and test everything:**

```bash
./run.sh
```

This script will:
1. ✅ Build the project
2. ✅ Flash firmware to ESP32
3. ✅ Wait for device to boot
4. ✅ Connect to ESP32 Access Point
5. ✅ Test all provisioning endpoints

## Prerequisites

1. **ESP-IDF Installation**: ESP-IDF v5.0+ installed and environment set up
   - The `run.sh` script will automatically source the ESP-IDF environment

2. **Device Keys**: Generate private key and CSR using your key generation script
   - Update `main/device_keys.h` with your generated keys
   - Replace `DEVICE_PRIVATE_KEY_PEM` and `DEVICE_CSR_PEM` placeholders

3. **Backend Server**: Your backend must implement:
   - `POST /sign-csr` endpoint
   - Accepts: `{"device_id": "...", "csr": "..."}`
   - Returns: `{"certificate": {"content": "..."}, "ca_certificate": {"content": "..."}}`
   - Requires: `Authorization: Bearer <provisioning_token>` header

4. **MQTT Broker**: MQTT broker with mTLS support (e.g., EMQX)

## Configuration

Configure the project using `idf.py menuconfig`:

### WiFi Provisioning Configuration
- **AP SSID Prefix**: SSID prefix for provisioning AP (default: "ESP32-Prov")
- **AP Password**: Password for provisioning AP (default: "prov12345678")

### Backend Configuration
- **Backend URL**: Base URL of your backend server (default: "https://your-backend.com")

### MQTT Configuration
- **MQTT Broker URI**: MQTT broker URI with mTLS (default: "mqtts://your-broker.com:8883")

## Setup Instructions

### 1. Generate Device Keys

Use your key generation script to create:
- Private key (PEM format)
- Certificate Signing Request (CSR, PEM format)

### 2. Update device_keys.h

Edit `main/device_keys.h` and replace the placeholder values:

```c
#define DEVICE_ID "device_0070"  // Your device ID
#define DEVICE_PRIVATE_KEY_PEM \
    "-----BEGIN PRIVATE KEY-----\n" \
    "Your actual private key here...\n" \
    "-----END PRIVATE KEY-----\n"

#define DEVICE_CSR_PEM \
    "-----BEGIN CERTIFICATE REQUEST-----\n" \
    "Your actual CSR here...\n" \
    "-----END CERTIFICATE REQUEST-----\n"
```

### 3. Configure Project

```bash
idf.py set-target esp32  # or esp32s2, esp32c3, etc.
idf.py menuconfig
```

Configure:
- Backend URL
- MQTT Broker URI
- AP SSID and password (optional)

### 4. Run Everything

Simply run the unified script:

```bash
./run.sh
```

This will build, flash, connect, and test everything automatically.

**Manual build/flash (if needed):**
```bash
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (e.g., `/dev/ttyACM0` or `COM3`).

## Provisioning Flow

### Stage 1: Initial Boot (Not Provisioned)

1. Device boots and checks NVS for provisioning status
2. If not provisioned, starts WiFi AP mode
3. HTTP server starts on port 80
4. AP SSID: `<AP_SSID_PREFIX>` (default: "ESP32-Prov")
5. AP Password: `<AP_PASSWORD>` (default: "prov12345678")

### Stage 2: WiFi Provisioning

1. **Connect to AP**: Connect your phone/laptop to the ESP32 AP
2. **Scan Networks**: 
   ```bash
   curl http://192.168.4.1/local-wifi
   ```
   Returns JSON array of available WiFi networks

3. **Submit Credentials**:
   ```bash
  curl -X POST http://192.168.4.1/provision \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_BEARER_TOKEN" \
  -d '{
    "ssid": "Statsnapp",
    "password": "1q2w3e4r",
    "device_id": "device_0070",
    "provisioning_token": "your_prov_token"
  }''
   ```
   
   **Important**: The `Authorization: Bearer <token>` header contains the Bearer token 
   that the ESP32 will use for authenticated API calls to the server on behalf of the user.
   This token is generated by the backend and passed through the frontend to the device.

4. Device saves credentials to NVS
5. Device switches to STA mode and connects to WiFi
6. Device gets IP address via DHCP

### Stage 3: CSR Submission

1. Device automatically submits CSR to backend:
   - URL: `<BACKEND_URL>/sign-csr`
   - Method: POST
   - Headers: `Authorization: Bearer <provisioning_token>`
   - Body: `{"device_id": "...", "csr": "..."}`

2. Backend signs CSR and returns certificates

3. Device saves certificates to NVS:
   - Device certificate
   - CA certificate

### Stage 4: MQTT Connection

1. Device loads certificates from NVS
2. Device loads private key from `device_keys.h`
3. Device connects to MQTT broker using mTLS
4. Device publishes status messages periodically

## API Endpoints

### GET /local-wifi

Scans for available WiFi networks.

**Response:**
```json
{
  "networks": [
    {
      "ssid": "NetworkName",
      "rssi": -45,
      "channel": 6,
      "secure": true
    }
  ],
  "count": 5
}
```

### POST /provision

Submits WiFi credentials, provisioning token, and receives Bearer token for server authentication.

**Request Headers:**
- `Content-Type: application/json`
- `Authorization: Bearer <bearer_token>` - Bearer token for ESP32 to use for authenticated API calls

**Request Body:**
```json
{
  "ssid": "YourWiFiSSID",
  "password": "YourWiFiPassword",
  "device_id": "device_0070",
  "provisioning_token": "your_provisioning_token_here"
}
```

**Note**: The Bearer token in the Authorization header is stored separately from the provisioning_token.
The provisioning_token is used for CSR signing, while the Bearer token is used for general
authenticated API calls to the server on behalf of the user.

**Response:**
```json
{
  "status": "ok",
  "message": "Credentials saved"
}
```

### GET /status

Returns current provisioning status.

**Response:**
```json
{
  "status": "provisioning|connected|disconnected",
  "ip": "192.168.1.100"
}
```

## State Machine

The application uses a state machine to manage the provisioning flow:

```
INIT → CHECK_PROVISIONING → [Provisioned?]
                              ├─ NO → AP_MODE → [Credentials received]
                              │                  └─ WIFI_CONNECTING → WIFI_CONNECTED
                              └─ YES → WIFI_CONNECTING → WIFI_CONNECTED
                                                          ↓
                                    CHECK_CERTIFICATES → [Certs exist?]
                                                          ├─ YES → MQTT_CONNECTING → MQTT_CONNECTED
                                                          └─ NO → SUBMIT_CSR → MQTT_CONNECTING → MQTT_CONNECTED
```

## Troubleshooting

### Device doesn't start AP mode
- Check serial output for errors
- Verify NVS initialization succeeded
- Ensure WiFi is supported on your chip

### Can't connect to AP
- Verify SSID and password are correct
- Check that channel is valid (1-13 for 2.4GHz)
- Ensure password is at least 8 characters

### CSR submission fails
- Check backend URL is correct
- Verify provisioning token is valid
- Check backend logs for errors
- Ensure device has internet connectivity

### MQTT connection fails
- Verify certificates are stored in NVS
- Check MQTT broker URI is correct
- Ensure broker supports mTLS
- Check broker logs for connection attempts

### Certificates not found
- Device will automatically submit CSR if certificates are missing
- Check backend is accessible from device
- Verify CSR format is correct

## Development Notes

- Certificates are stored in NVS namespace `device_config`
- WiFi credentials are stored in NVS
- State machine runs in a FreeRTOS task
- HTTP server runs on port 80
- MQTT client uses mTLS (mqtts://) protocol

## License

This example code is in the Public Domain (CC0 licensed).


for logging
   cd ~/Desktop/statsclient
   source esp-idf/export.sh
   idf.py -p /dev/ttyACM0 monitor