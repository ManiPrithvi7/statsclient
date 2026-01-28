/* WiFi Provisioning Module Implementation - Arduino Version
 *
 * Handles WiFi Access Point mode for device provisioning.
 * Provides HTTP endpoints for WiFi scan and credential submission.
 */

#include "WiFiProvisioning.h"

// Static member initialization
AsyncWebServer* WiFiProvisioning::server = nullptr;
bool WiFiProvisioning::provisioningActive = false;
bool WiFiProvisioning::wifiConnected = false;
char WiFiProvisioning::staIp[16] = {0};

// WiFi scan cache
WiFiProvisioning::WiFiNetworkInfo WiFiProvisioning::cachedNetworks[WIFI_SCAN_MAX_APS];
int WiFiProvisioning::cachedNetworkCount = 0;
bool WiFiProvisioning::initialScanDone = false;

int WiFiProvisioning::start(const char* ssidPrefix, const char* password) {
    if (provisioningActive) {
        Serial.println("WARNING: Provisioning already active");
        return 0;
    }
    
    Serial.println("Starting WiFi provisioning");
    
    // Register WiFi event handler
    WiFi.onEvent(WiFiProvisioning::onWiFiEvent);
    
    // Configure WiFi AP
    WiFi.mode(WIFI_AP_STA);
    String apSSID = String(ssidPrefix);
    
    if (strlen(password) == 0) {
        WiFi.softAP(apSSID.c_str());
        Serial.print("WiFi AP started (OPEN): ");
    } else {
        WiFi.softAP(apSSID.c_str(), password);
        Serial.print("WiFi AP started (WPA2): ");
    }
    Serial.println(apSSID);
    
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    
    // Perform initial WiFi scan
    Serial.println("Performing initial WiFi scan...");
    performWiFiScan();
    
    // Start HTTP server
    server = new AsyncWebServer(80);
    
    // Register handlers
    server->on("/", HTTP_GET, handleRoot);
    server->on("/local-wifi", HTTP_GET, handleScan);
    server->on("/provision", HTTP_POST, [](AsyncWebServerRequest* request){}, NULL, handleProvision);
    server->on("/status", HTTP_GET, handleStatus);
    server->on("/local-wifi", HTTP_OPTIONS, handleOptions);
    server->on("/provision", HTTP_OPTIONS, handleOptions);
    server->on("/status", HTTP_OPTIONS, handleOptions);
    
    server->begin();
    Serial.println("HTTP server started on port 80");
    
    provisioningActive = true;
    Serial.println("========================================");
    Serial.println("WiFi provisioning started successfully");
    Serial.println("========================================");
    
    return 0;
}

int WiFiProvisioning::stop() {
    if (!provisioningActive) {
        return 0;
    }
    
    Serial.println("Stopping WiFi provisioning");
    
    if (server) {
        server->end();
        delete server;
        server = nullptr;
    }
    
    initialScanDone = false;
    cachedNetworkCount = 0;
    provisioningActive = false;
    
    return 0;
}

bool WiFiProvisioning::isProvisioned() {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        return false;
    }
    
    bool provisioned = prefs.getBool("provisioned", false);
    prefs.end();
    
    return provisioned;
}

bool WiFiProvisioning::isActive() {
    return provisioningActive;
}

int WiFiProvisioning::connectToWiFi() {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        Serial.println("ERROR: Failed to open Preferences");
        return -1;
    }
    
    String ssid = prefs.getString("wifi_ssid", "");
    String password = prefs.getString("wifi_pass", "");
    prefs.end();
    
    if (ssid.length() == 0) {
        Serial.println("ERROR: WiFi credentials not found");
        return -1;
    }
    
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Wait a bit for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
        if (attempts % 10 == 0) {
            Serial.print(".");
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        IPAddress ip = WiFi.localIP();
        snprintf(staIp, sizeof(staIp), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        return 0;
    }
    
    return -1;
}

int WiFiProvisioning::getBearerToken(char* token, size_t tokenLen) {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        return -1;
    }
    
    String tokenStr = prefs.getString("bearer_token", "");
    prefs.end();
    
    if (tokenStr.length() == 0) {
        return -1;
    }
    
    tokenStr.toCharArray(token, tokenLen);
    return 0;
}

int WiFiProvisioning::clearAndRestart() {
    Serial.println("========================================");
    Serial.println("Clearing provisioning credentials");
    Serial.println("Returning to AP mode");
    Serial.println("========================================");
    
    stop();
    
    Preferences prefs;
    if (prefs.begin("device_config", false)) {
        prefs.remove("provisioned");
        prefs.remove("wifi_ssid");
        prefs.remove("wifi_pass");
        prefs.remove("device_id");
        prefs.remove("prov_token");
        prefs.remove("bearer_token");
        prefs.end();
        Serial.println("✓ Provisioning data cleared");
    }
    
    WiFi.disconnect();
    wifiConnected = false;
    memset(staIp, 0, sizeof(staIp));
    
    delay(1000);
    
    // Get AP credentials from config
    const char* apSSID = "ESP32-Prov";  // Default, should be configurable
    const char* apPassword = "prov12345678";  // Default, should be configurable
    
    return start(apSSID, apPassword);
}

bool WiFiProvisioning::getStatus(char* ipAddr, size_t ipLen) {
    if (wifiConnected && ipAddr && ipLen > 0) {
        strncpy(ipAddr, staIp, ipLen - 1);
        ipAddr[ipLen - 1] = '\0';
        return true;
    }
    return false;
}

void WiFiProvisioning::handleRoot(AsyncWebServerRequest* request) {
    logRequest(request);
    
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
        "{\"status\":\"ok\",\"message\":\"ESP32 Provisioning Server\",\"endpoints\":[\"/local-wifi\",\"/provision\",\"/status\"]}");
    setCORSHeaders(response, request);
    logResponse("GET", request->url().c_str(), 200, "{\"status\":\"ok\",...}");
    request->send(response);
}

void WiFiProvisioning::handleScan(AsyncWebServerRequest* request) {
    logRequest(request);
    
    // Check for refresh parameter
    bool forceRefresh = false;
    if (request->hasParam("refresh")) {
        String refresh = request->getParam("refresh")->value();
        if (refresh == "true" || refresh == "1") {
            forceRefresh = true;
        }
    }
    
    // Perform scan if needed
    if (!initialScanDone || forceRefresh) {
        performWiFiScan();
    }
    
    // Build JSON response
    DynamicJsonDocument doc(4096);
    JsonArray networks = doc.createNestedArray("networks");
    
    for (int i = 0; i < cachedNetworkCount; i++) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = cachedNetworks[i].ssid;
        network["rssi"] = cachedNetworks[i].rssi;
        network["channel"] = cachedNetworks[i].channel;
        network["secure"] = cachedNetworks[i].secure;
    }
    
    doc["count"] = cachedNetworkCount;
    doc["cached"] = !forceRefresh;
    
    String response;
    serializeJson(doc, response);
    
    AsyncWebServerResponse* httpResponse = request->beginResponse(200, "application/json", response);
    setCORSHeaders(httpResponse, request);
    logResponse("GET", request->url().c_str(), 200, response.c_str());
    request->send(httpResponse);
}

void WiFiProvisioning::handleProvision(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    // Log request only on first chunk
    if (index == 0) {
        logRequest(request);
    }
    
    // Accumulate body data across chunks
    static String bodyBuffer = "";
    if (index == 0) {
        bodyBuffer = "";
    }
    
    // Append this chunk to buffer
    for (size_t i = 0; i < len; i++) {
        bodyBuffer += (char)data[i];
    }
    
    // Only process when we have the complete body
    if (index + len < total) {
        return; // Wait for more data
    }
    
    // Now we have the complete body - process it
    Serial.print("Complete body received: ");
    Serial.print(bodyBuffer.length());
    Serial.println(" bytes");
    
    // Extract Authorization header
    String bearerToken = "";
    if (request->hasHeader("Authorization")) {
        String authHeader = request->header("Authorization");
        if (authHeader.startsWith("Bearer ")) {
            bearerToken = authHeader.substring(7);
        } else {
            bearerToken = authHeader;
        }
    }
    
    // Parse JSON body
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, bodyBuffer);
    
    if (error) {
        Serial.print("JSON parse error: ");
        Serial.println(error.c_str());
        String errorResponse = "{\"error\":\"invalid_json\"}";
        AsyncWebServerResponse* response = request->beginResponse(400, "application/json", errorResponse);
        setCORSHeaders(response, request);
        logResponse("POST", request->url().c_str(), 400, errorResponse.c_str());
        request->send(response);
        bodyBuffer = ""; // Clear buffer
        return;
    }
    
    // Validate required fields
    if (!doc.containsKey("ssid") || !doc.containsKey("password") || 
        !doc.containsKey("device_id") || !doc.containsKey("provisioning_token")) {
        String errorResponse = "{\"error\":\"missing_fields\"}";
        AsyncWebServerResponse* response = request->beginResponse(400, "application/json", errorResponse);
        setCORSHeaders(response, request);
        logResponse("POST", request->url().c_str(), 400, errorResponse.c_str());
        request->send(response);
        bodyBuffer = ""; // Clear buffer
        return;
    }
    
    String ssid = doc["ssid"].as<String>();
    String password = doc["password"].as<String>();
    String deviceId = doc["device_id"].as<String>();
    String provToken = doc["provisioning_token"].as<String>();
    
    Serial.print("Received credentials - SSID: ");
    Serial.print(ssid);
    Serial.print(", Device ID: ");
    Serial.println(deviceId);
    
    // Save credentials
    if (saveWiFiCredentials(ssid.c_str(), password.c_str(), deviceId.c_str(), 
                           provToken.c_str(), bearerToken.length() > 0 ? bearerToken.c_str() : nullptr) != 0) {
        String errorResponse = "{\"error\":\"save_failed\"}";
        AsyncWebServerResponse* response = request->beginResponse(500, "application/json", errorResponse);
        setCORSHeaders(response, request);
        logResponse("POST", request->url().c_str(), 500, errorResponse.c_str());
        request->send(response);
        bodyBuffer = ""; // Clear buffer
        return;
    }
    
    // Send success response
    String successResponse = "{\"status\":\"ok\",\"message\":\"Credentials saved\"}";
    AsyncWebServerResponse* response = request->beginResponse(200, "application/json", successResponse);
    setCORSHeaders(response, request);
    logResponse("POST", request->url().c_str(), 200, successResponse.c_str());
    request->send(response);
    
    // Clear buffer
    bodyBuffer = "";
    
    // Delay stopping to ensure response is sent
    // Use a callback or delay to stop after response is sent
    Serial.println("Stopping provisioning and preparing for WiFi connection...");
    delay(100); // Small delay to ensure response is sent
    stop();
}

void WiFiProvisioning::handleStatus(AsyncWebServerRequest* request) {
    logRequest(request);
    
    DynamicJsonDocument doc(256);
    
    if (wifiConnected) {
        doc["status"] = "connected";
        doc["ip"] = staIp;
    } else if (provisioningActive) {
        doc["status"] = "provisioning";
        doc["ip"] = "192.168.4.1";
    } else {
        doc["status"] = "disconnected";
    }
    
    String response;
    serializeJson(doc, response);
    
    AsyncWebServerResponse* httpResponse = request->beginResponse(200, "application/json", response);
    setCORSHeaders(httpResponse, request);
    logResponse("GET", request->url().c_str(), 200, response.c_str());
    request->send(httpResponse);
}

void WiFiProvisioning::handleOptions(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(204);
    setCORSHeaders(response, request);
    logResponse("OPTIONS", request->url().c_str(), 204, "(empty - CORS preflight)");
    request->send(response);
}

void WiFiProvisioning::setCORSHeaders(AsyncWebServerResponse* response, AsyncWebServerRequest* request) {
    // Get Origin header
    String origin = "";
    if (request->hasHeader("Origin")) {
        origin = request->header("Origin");
    }
    
    // Check if origin is allowed
    String allowedOrigin = "*";
    if (origin.indexOf("localhost:3000") >= 0 || origin.indexOf("127.0.0.1:3000") >= 0) {
        allowedOrigin = origin;
    } else if (origin.indexOf("statsnapp.vercel.app") >= 0) {
        allowedOrigin = origin;
    }
    
    response->addHeader("Access-Control-Allow-Origin", allowedOrigin);
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    response->addHeader("Access-Control-Max-Age", "3600");
}

int WiFiProvisioning::performWiFiScan() {
    Serial.println("Performing WiFi scan...");
    
    int n = WiFi.scanNetworks();
    
    if (n == 0) {
        Serial.println("No networks found");
        cachedNetworkCount = 0;
        initialScanDone = true;
        return 0;
    }
    
    cachedNetworkCount = (n < WIFI_SCAN_MAX_APS) ? n : WIFI_SCAN_MAX_APS;
    
    for (int i = 0; i < cachedNetworkCount; i++) {
        cachedNetworks[i].ssid = WiFi.SSID(i);
        cachedNetworks[i].rssi = WiFi.RSSI(i);
        cachedNetworks[i].channel = WiFi.channel(i);
        cachedNetworks[i].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    
    initialScanDone = true;
    Serial.print("WiFi scan completed: ");
    Serial.print(cachedNetworkCount);
    Serial.println(" networks cached");
    
    return 0;
}

int WiFiProvisioning::saveWiFiCredentials(const char* ssid, const char* password,
                                          const char* deviceId, const char* provToken,
                                          const char* bearerToken) {
    Preferences prefs;
    if (!prefs.begin("device_config", false)) {
        Serial.println("ERROR: Failed to open Preferences");
        return -1;
    }
    
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", password);
    prefs.putString("device_id", deviceId);
    prefs.putString("prov_token", provToken);
    
    if (bearerToken && strlen(bearerToken) > 0) {
        prefs.putString("bearer_token", bearerToken);
        Serial.println("Bearer token saved");
    }
    
    prefs.putBool("provisioned", true);
    prefs.end();
    
    Serial.println("✓ Credentials saved to Preferences");
    return 0;
}

void WiFiProvisioning::logRequest(AsyncWebServerRequest* request) {
    Serial.println("");
    Serial.println("========================================");
    Serial.println(">>> INCOMING HTTP REQUEST");
    Serial.println("========================================");
    Serial.print("Method: ");
    Serial.println(request->methodToString());
    Serial.print("URI: ");
    Serial.println(request->url());
    
    if (request->hasParam("refresh")) {
        Serial.print("Query: refresh=");
        Serial.println(request->getParam("refresh")->value());
    }
    
    if (request->hasHeader("User-Agent")) {
        Serial.print("User-Agent: ");
        Serial.println(request->header("User-Agent"));
    }
    
    if (request->hasHeader("Authorization")) {
        String auth = request->header("Authorization");
        if (auth.length() > 50) {
            auth = auth.substring(0, 50) + "...";
        }
        Serial.print("Authorization: ");
        Serial.println(auth);
    } else {
        Serial.println("Authorization: (not present)");
    }
    
    if (request->hasHeader("Origin")) {
        Serial.print("Origin: ");
        Serial.println(request->header("Origin"));
    }
    
    Serial.println("========================================");
    Serial.println("");
}

void WiFiProvisioning::logResponse(const char* method, const char* uri, int statusCode, const char* body) {
    Serial.println("");
    Serial.println("========================================");
    Serial.println("<<< OUTGOING HTTP RESPONSE");
    Serial.println("========================================");
    Serial.print("Method: ");
    Serial.println(method);
    Serial.print("URI: ");
    Serial.println(uri);
    Serial.print("HTTP Status: ");
    Serial.println(statusCode);
    
    if (body) {
        size_t bodyLen = strlen(body);
        Serial.print("Response Body Length: ");
        Serial.print(bodyLen);
        Serial.println(" bytes");
        
        if (bodyLen > 500) {
            char truncated[510];
            strncpy(truncated, body, 500);
            truncated[500] = '\0';
            strcat(truncated, "...");
            Serial.print("Response Body (first 500 chars): ");
            Serial.println(truncated);
        } else {
            Serial.print("Response Body: ");
            Serial.println(body);
        }
    }
    
    Serial.println("========================================");
    Serial.println("");
}

// WiFi event handler for connection status
void WiFiProvisioning::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
            wifiConnected = true;
            IPAddress ip = WiFi.localIP();
            snprintf(staIp, sizeof(staIp), "%d.%d.%d.%d",
                     ip[0], ip[1], ip[2], ip[3]);
            Serial.print("WiFi Connected! IP: ");
            Serial.println(staIp);
            break;
        }
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            wifiConnected = false;
            memset(staIp, 0, sizeof(staIp));
            Serial.println("WiFi Disconnected");
            break;
            
        default:
            break;
    }
}
