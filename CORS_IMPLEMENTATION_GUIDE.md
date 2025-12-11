# CORS Implementation Guide for ESP32 Provisioning

## Overview

This document provides guidance for firmware developers implementing CORS (Cross-Origin Resource Sharing) support in ESP32 provisioning endpoints. CORS headers are **required** for direct browser-to-device communication during the provisioning process.

---

## Why CORS is Required

### The Problem

- **Privacy**: WiFi credentials should not pass through cloud servers
- **Cloud Limitation**: Cloud servers cannot access local network devices (192.168.4.1)
- **Browser Security**: Browsers block cross-origin requests without CORS headers

### Current Behavior Without CORS

- ❌ Connection checks work (using `no-cors` mode, but can't read response)
- ❌ WiFi scanning fails (needs to read response)
- ❌ Provisioning fails (needs to read response)

### Expected Behavior With CORS

- ✅ All requests succeed
- ✅ Responses are fully readable
- ✅ Complete provisioning flow works end-to-end

---

## Required CORS Headers

All HTTP responses from ESP32 endpoints must include these headers:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type, Authorization
Access-Control-Max-Age: 3600
```

### Security Note

Using `Access-Control-Allow-Origin: *` is acceptable because:
- Device is on isolated AP network (192.168.4.1)
- Only accessible when user is connected to device AP
- No internet access during provisioning

For production, consider restricting to specific origins if needed.

---

## Implementation for ESP-IDF

### 1. CORS Helper Function

Add this function to your HTTP server code:

```c
#include <esp_http_server.h>

/**
 * @brief Add CORS headers to HTTP response
 * 
 * Adds required CORS headers to allow cross-origin requests from web browsers.
 * This is essential for direct client-to-device communication during provisioning.
 */
static esp_err_t add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "3600");
    return ESP_OK;
}
```

### 2. Update Existing Handlers

Call `add_cors_headers(req)` **before** sending any response in all handlers:

```c
static esp_err_t scan_handler(httpd_req_t *req)
{
    // ... your handler logic ...
    
    httpd_resp_set_type(req, "application/json");
    
    // Add CORS headers BEFORE sending response
    add_cors_headers(req);
    
    httpd_resp_sendstr(req, json_string);
    return ESP_OK;
}
```

### 3. Add OPTIONS Handlers (Preflight Requests)

Browsers send OPTIONS requests before POST requests. Add handlers for all endpoints:

```c
/**
 * @brief HTTP OPTIONS handler for /local-wifi endpoint (CORS preflight)
 */
static esp_err_t scan_options_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OPTIONS /local-wifi - CORS preflight request");
    add_cors_headers(req);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief HTTP OPTIONS handler for /provision endpoint (CORS preflight)
 */
static esp_err_t provision_options_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OPTIONS /provision - CORS preflight request");
    add_cors_headers(req);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief HTTP OPTIONS handler for /status endpoint (CORS preflight)
 */
static esp_err_t status_options_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OPTIONS /status - CORS preflight request");
    add_cors_headers(req);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
```

### 4. Register OPTIONS Handlers

Register OPTIONS handlers in your HTTP server setup:

```c
static httpd_handle_t start_http_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // ... configure server ...
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // GET /local-wifi
        httpd_uri_t scan_uri = {
            .uri = "/local-wifi",
            .method = HTTP_GET,
            .handler = scan_handler,
        };
        httpd_register_uri_handler(server, &scan_uri);

        // OPTIONS /local-wifi (CORS preflight)
        httpd_uri_t scan_options_uri = {
            .uri = "/local-wifi",
            .method = HTTP_OPTIONS,
            .handler = scan_options_handler,
        };
        httpd_register_uri_handler(server, &scan_options_uri);

        // POST /provision
        httpd_uri_t provision_uri = {
            .uri = "/provision",
            .method = HTTP_POST,
            .handler = provision_handler,
        };
        httpd_register_uri_handler(server, &provision_uri);

        // OPTIONS /provision (CORS preflight)
        httpd_uri_t provision_options_uri = {
            .uri = "/provision",
            .method = HTTP_OPTIONS,
            .handler = provision_options_handler,
        };
        httpd_register_uri_handler(server, &provision_options_uri);

        // GET /status
        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
        };
        httpd_register_uri_handler(server, &status_uri);

        // OPTIONS /status (CORS preflight)
        httpd_uri_t status_options_uri = {
            .uri = "/status",
            .method = HTTP_OPTIONS,
            .handler = status_options_handler,
        };
        httpd_register_uri_handler(server, &status_options_uri);
    }
    
    return server;
}
```

---

## Implementation Checklist

- [ ] Add `add_cors_headers()` helper function
- [ ] Add CORS headers to POST /provision handler
- [ ] Add CORS headers to GET /local-wifi handler
- [ ] Add CORS headers to GET /status handler
- [ ] Add CORS headers to all error responses
- [ ] Add OPTIONS handler for /provision
- [ ] Add OPTIONS handler for /local-wifi
- [ ] Add OPTIONS handler for /status
- [ ] Register all OPTIONS handlers in server setup
- [ ] Test with browser fetch API
- [ ] Verify responses are readable (not opaque)
- [ ] Update firmware version number

---

## Endpoints to Update

### 1. POST /provision

**Purpose:** Receive WiFi credentials and provisioning token

**Request Body:** JSON with `ssid`, `password`, `provisioning_token`, `device_id`

**Response:** JSON `{"status": "ok", "message": "Credentials saved"}`

**Required:**
- CORS headers in response
- OPTIONS handler for preflight

### 2. GET /local-wifi

**Purpose:** Return list of available WiFi networks

**Query Params:** `?refresh=true` (optional, triggers fresh scan)

**Response:** JSON `{"networks": [...], "count": 5, "cached": true}`

**Required:**
- CORS headers in response
- OPTIONS handler for preflight

### 3. GET /status

**Purpose:** Get provisioning status

**Response:** JSON `{"status": "provisioning|connected|disconnected", "ip": "192.168.4.1"}`

**Required:**
- CORS headers in response
- OPTIONS handler for preflight

---

## Testing Instructions

### 1. Flash Firmware

Flash the updated firmware to your ESP32 device.

### 2. Connect to Device AP

Connect your computer/phone to the ESP32 Access Point (default: "ESP32-Prov").

### 3. Open Browser Console

Open browser developer tools (F12) and navigate to the Console tab.

### 4. Test WiFi Scan

```javascript
fetch('http://192.168.4.1/local-wifi')
  .then(r => r.json())
  .then(data => console.log('Success:', data))
  .catch(err => console.error('Error:', err))
```

**Expected Result:** Should return network list without CORS errors.

### 5. Test Provisioning

```javascript
fetch('http://192.168.4.1/provision', {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json',
    'Authorization': 'Bearer your_token_here'
  },
  body: JSON.stringify({
    ssid: 'YourWiFi',
    password: 'YourPassword',
    device_id: 'device_0070',
    provisioning_token: 'your_provisioning_token'
  })
})
  .then(r => r.json())
  .then(data => console.log('Success:', data))
  .catch(err => console.error('Error:', err))
```

**Expected Result:** Should return success response without CORS errors.

### 6. Verify Preflight Requests

Check the Network tab in browser dev tools. You should see:
- OPTIONS request to `/provision` (preflight)
- POST request to `/provision` (actual request)

Both should return 200 OK with CORS headers.

---

## Common Issues

### Issue: CORS errors in browser console

**Symptoms:**
```
Access to fetch at 'http://192.168.4.1/local-wifi' from origin 'null' 
has been blocked by CORS policy: No 'Access-Control-Allow-Origin' header is present.
```

**Solution:**
- Verify `add_cors_headers()` is called before `httpd_resp_sendstr()`
- Check that headers are set correctly
- Ensure OPTIONS handlers are registered

### Issue: Preflight request fails

**Symptoms:**
- OPTIONS request returns 404 or 405
- Browser shows CORS error

**Solution:**
- Register OPTIONS handlers for all endpoints
- Ensure OPTIONS handlers return 200 OK with CORS headers

### Issue: Response is opaque

**Symptoms:**
- Request succeeds but response body is empty/null
- Can't read response in JavaScript

**Solution:**
- Ensure CORS headers are present
- Check that `Access-Control-Allow-Origin` is set to `*` or specific origin
- Verify response content-type is set correctly

---

## Quick Reference

### CORS Headers Function

```c
static esp_err_t add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "3600");
    return ESP_OK;
}
```

**Call this function BEFORE sending any response in all handlers.**

---

## Support

If you have questions or need clarification, please contact the development team.

---

## References

- [MDN: CORS](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS)
- [ESP-IDF HTTP Server API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html)

