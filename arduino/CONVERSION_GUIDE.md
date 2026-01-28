# ESP-IDF to Arduino Conversion Guide

This document explains the key differences and conversion mappings between the ESP-IDF and Arduino versions.

## API Mappings

### Storage (NVS → Preferences)

| ESP-IDF | Arduino |
|---------|---------|
| `nvs_flash_init()` | `Preferences.begin()` |
| `nvs_open()` | `Preferences.begin()` |
| `nvs_get_str()` | `prefs.getString()` |
| `nvs_set_str()` | `prefs.putString()` |
| `nvs_get_u8()` | `prefs.getBool()` / `prefs.getUChar()` |
| `nvs_set_u8()` | `prefs.putBool()` / `prefs.putUChar()` |
| `nvs_commit()` | Automatic (no need to call) |
| `nvs_close()` | `prefs.end()` |

### HTTP Server

| ESP-IDF | Arduino |
|---------|---------|
| `esp_http_server.h` | `ESPAsyncWebServer.h` |
| `httpd_handle_t` | `AsyncWebServer*` |
| `httpd_req_t*` | `AsyncWebServerRequest*` |
| `httpd_resp_set_type()` | `response->setContentType()` |
| `httpd_resp_set_hdr()` | `response->addHeader()` |
| `httpd_resp_sendstr()` | `request->send(response)` |
| `httpd_resp_set_status()` | `response->setCode()` |
| `httpd_req_recv()` | Handler receives data in callback |
| `httpd_register_uri_handler()` | `server->on()` |

### WiFi

| ESP-IDF | Arduino |
|---------|---------|
| `esp_wifi.h` | `WiFi.h` |
| `esp_wifi_init()` | Automatic |
| `esp_wifi_set_mode()` | `WiFi.mode()` |
| `esp_wifi_set_config()` | `WiFi.softAP()` / `WiFi.begin()` |
| `esp_wifi_start()` | Automatic |
| `esp_wifi_scan_start()` | `WiFi.scanNetworks()` |
| `wifi_ap_record_t` | `WiFi.SSID()`, `WiFi.RSSI()`, etc. |
| `WIFI_EVENT` | `WiFi.onEvent()` |

### HTTP Client

| ESP-IDF | Arduino |
|---------|---------|
| `esp_http_client.h` | `HTTPClient.h` + `WiFiClientSecure.h` |
| `esp_http_client_config_t` | `HTTPClient.begin()` + `WiFiClientSecure` |
| `esp_http_client_perform()` | `https.POST()` / `https.GET()` |
| `esp_http_client_get_status_code()` | `https.responseCode()` |
| `esp_http_client_get_content_length()` | `https.getSize()` |
| Event handler pattern | Direct return values |

### MQTT

| ESP-IDF | Arduino |
|---------|---------|
| `mqtt_client.h` | `PubSubClient.h` |
| `esp_mqtt_client_handle_t` | `PubSubClient*` |
| `esp_mqtt_client_init()` | `new PubSubClient()` |
| `esp_mqtt_client_start()` | `mqtt->connect()` |
| `esp_mqtt_client_publish()` | `mqtt->publish()` |
| `esp_mqtt_client_subscribe()` | `mqtt->subscribe()` |
| Event handler pattern | `mqtt->setCallback()` + `mqtt->loop()` |

### JSON

| ESP-IDF | Arduino |
|---------|---------|
| `cJSON.h` | `ArduinoJson.h` |
| `cJSON_Parse()` | `deserializeJson()` |
| `cJSON_Print()` | `serializeJson()` |
| `cJSON_GetObjectItem()` | `doc["key"]` |
| `cJSON_AddStringToObject()` | `doc["key"] = value` |
| `cJSON_CreateObject()` | `DynamicJsonDocument doc(size)` |
| `cJSON_CreateArray()` | `doc.createNestedArray()` |

### Logging

| ESP-IDF | Arduino |
|---------|---------|
| `esp_log.h` | `Serial` |
| `ESP_LOGI()` | `Serial.println()` |
| `ESP_LOGE()` | `Serial.println("ERROR: ...")` |
| `ESP_LOGW()` | `Serial.println("WARNING: ...")` |
| `ESP_LOGD()` | `Serial.println()` (if debug enabled) |

### Timing

| ESP-IDF | Arduino |
|---------|---------|
| `esp_timer_get_time()` | `micros()` |
| `vTaskDelay()` | `delay()` |
| `pdMS_TO_TICKS()` | Direct milliseconds |

### Error Handling

| ESP-IDF | Arduino |
|---------|---------|
| `esp_err_t` | `int` (0 = success, -1 = error) |
| `ESP_OK` | `0` |
| `ESP_FAIL` | `-1` |
| `ESP_ERR_*` | Negative error codes |

## Code Structure Changes

### Function Signatures

**ESP-IDF:**
```c
esp_err_t wifi_provisioning_start(void);
```

**Arduino:**
```cpp
static int WiFiProvisioning::start(const char* ssidPrefix, const char* password);
```

### HTTP Handler Pattern

**ESP-IDF:**
```c
static esp_err_t scan_handler(httpd_req_t *req) {
    // Read request
    // Process
    // Send response
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}
```

**Arduino:**
```cpp
static void WiFiProvisioning::handleScan(AsyncWebServerRequest* request) {
    // Process (request already parsed)
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", json);
    setCORSHeaders(response, request);
    request->send(response);
}
```

### Async vs Sync

- **ESP-IDF**: Synchronous handlers (blocking)
- **Arduino**: Asynchronous handlers (non-blocking)

### Memory Management

- **ESP-IDF**: Manual malloc/free
- **Arduino**: String class, automatic cleanup

## Key Implementation Notes

1. **CORS Headers**: Same logic, but using `response->addHeader()` instead of `httpd_resp_set_hdr()`

2. **WiFi Scan**: `WiFi.scanNetworks()` is blocking, so we cache results

3. **POST Body**: ESPAsyncWebServer provides body in callback parameters, not via `recv()`

4. **State Machine**: Same logic, but using `millis()` for timing instead of FreeRTOS ticks

5. **Preferences**: Automatically commits, no need for explicit commit calls

6. **MQTT Loop**: Must call `mqtt->loop()` regularly in main loop

## Testing Checklist

- [ ] WiFi AP starts correctly
- [ ] HTTP server responds to all endpoints
- [ ] CORS headers are set correctly
- [ ] WiFi scan returns networks
- [ ] Provisioning saves credentials
- [ ] WiFi connection works
- [ ] Internet verification works
- [ ] CSR submission works
- [ ] Certificates are saved/loaded
- [ ] MQTT connection with mTLS works
- [ ] All logging works correctly
