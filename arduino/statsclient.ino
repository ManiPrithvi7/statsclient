/* WiFi Provisioning with mTLS MQTT - Arduino Main Application
 *
 * This application implements a complete provisioning flow:
 * 1. Boot → Check if device is provisioned
 * 2. If not provisioned → Start AP mode with HTTP server for provisioning
 * 3. After provisioning → Connect to WiFi
 * 4. Submit CSR to backend and receive certificates
 * 5. Connect to MQTT broker using mTLS
 */

#include "WiFiProvisioning.h"
#include "CertificateManager.h"
#include "InternetVerification.h"
#include "MQTTHandler.h"
#include "DeviceKeys.h"
#include <Preferences.h>
#include <WiFi.h>

// Configuration - Update these values
#define AP_SSID_PREFIX "ESP32-Prov"
#define AP_PASSWORD "prov12345678"
#define BACKEND_URL "https://your-backend-url.com"
#define MQTT_BROKER "your-mqtt-broker.com"
#define MQTT_PORT 8883

// Application states
enum AppState {
    APP_STATE_INIT,
    APP_STATE_CHECK_PROVISIONING,
    APP_STATE_AP_MODE,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_CHECK_CERTIFICATES,
    APP_STATE_SUBMIT_CSR,
    APP_STATE_MQTT_CONNECTING,
    APP_STATE_MQTT_CONNECTED,
    APP_STATE_ERROR
};

AppState appState = APP_STATE_INIT;
unsigned long lastStateChange = 0;
unsigned long stateTimeout = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("========================================");
    Serial.println("=== WiFi Provisioning with mTLS MQTT ===");
    Serial.println("========================================");
    
    // Initialize Preferences (replaces NVS) - just test if it works
    Preferences prefs;
    if (!prefs.begin("device_config", false)) {
        Serial.println("ERROR: Failed to initialize Preferences");
        return;
    }
    prefs.end();
    
    // DEVELOPMENT MODE: Clear all provisioning data on every boot
    Serial.println("========================================");
    Serial.println("DEVELOPMENT MODE: Clearing provisioning");
    Serial.println("========================================");
    clearProvisioningData();
    
    
    appState = APP_STATE_CHECK_PROVISIONING;
    lastStateChange = millis();
}

void loop() {
    unsigned long now = millis();
    
    // Process MQTT messages if connected
    if (appState == APP_STATE_MQTT_CONNECTED) {
        MQTTHandler::loop();
    }
    
    // State machine
    switch (appState) {
        case APP_STATE_INIT:
            appState = APP_STATE_CHECK_PROVISIONING;
            break;
            
        case APP_STATE_CHECK_PROVISIONING:
            if (WiFiProvisioning::isProvisioned()) {
                Serial.println("Device is provisioned, connecting to WiFi...");
                appState = APP_STATE_WIFI_CONNECTING;
            } else {
                Serial.println("Device not provisioned, starting AP mode...");
                appState = APP_STATE_AP_MODE;
            }
            break;
            
        case APP_STATE_AP_MODE:
            if (!WiFiProvisioning::isProvisioned()) {
                if (!WiFiProvisioning::isActive()) {
                    if (WiFiProvisioning::start(AP_SSID_PREFIX, AP_PASSWORD) != 0) {
                        Serial.println("ERROR: Failed to start provisioning");
                        delay(5000);
                    } else {
                        Serial.println("Provisioning AP active. Waiting for credentials...");
                    }
                }
            } else {
                Serial.println("Device is provisioned, moving to WiFi connecting state");
                appState = APP_STATE_WIFI_CONNECTING;
            }
            delay(2000);
            break;
            
        case APP_STATE_WIFI_CONNECTING:
            {
                static bool connectionAttempted = false;
                
                if (!connectionAttempted) {
                    if (WiFiProvisioning::connectToWiFi() == 0) {
                        connectionAttempted = true;
                        stateTimeout = now + 30000; // 30 second timeout
                    }
                }
                
                if (WiFi.status() == WL_CONNECTED) {
                Serial.println("========================================");
                Serial.println("WiFi Connected!");
                Serial.print("IP Address: ");
                Serial.println(WiFi.localIP());
                Serial.println("========================================");
                connectionAttempted = false;
                appState = APP_STATE_WIFI_CONNECTED;
            } else if (now > stateTimeout) {
                    Serial.println("ERROR: WiFi connection timeout");
                    connectionAttempted = false;
                    appState = APP_STATE_ERROR;
                }
            }
            delay(1000);
            break;
            
        case APP_STATE_WIFI_CONNECTED:
            {
                static bool verificationDone = false;
                static int verificationRetries = 0;
                const int MAX_VERIFICATION_RETRIES = 2;
                
                if (!WiFiProvisioning::isProvisioned()) {
                    Serial.println("Device not provisioned, resetting to AP mode");
                    verificationDone = false;
                    verificationRetries = 0;
                    appState = APP_STATE_AP_MODE;
                    break;
                }
                
                if (!verificationDone) {
                    Serial.println("WiFi connected - verifying internet access...");
                    delay(2000); // Wait for network to stabilize
                    
                    if (InternetVerification::test() == 0) {
                        Serial.println("========================================");
                        Serial.println("✓ Internet connectivity verified!");
                        Serial.println("========================================");
                        verificationDone = true;
                        verificationRetries = 0;
                    } else {
                        verificationRetries++;
                        Serial.print("✗ Internet verification failed! Retry: ");
                        Serial.print(verificationRetries);
                        Serial.print("/");
                        Serial.println(MAX_VERIFICATION_RETRIES);
                        
                        if (verificationRetries >= MAX_VERIFICATION_RETRIES) {
                            Serial.println("Maximum retries reached. Proceeding anyway...");
                            verificationDone = true;
                            verificationRetries = 0;
                        } else {
                            delay(5000);
                            break;
                        }
                    }
                }
                
                if (verificationDone) {
                    appState = APP_STATE_CHECK_CERTIFICATES;
                }
            }
            break;
            
        case APP_STATE_CHECK_CERTIFICATES:
            Serial.println("========================================");
            Serial.println("Checking if certificates exist...");
            Serial.println("========================================");
            
            if (CertificateManager::hasCertificates()) {
                Serial.println("✓ Certificates found");
                Serial.println("Proceeding to MQTT connection...");
                appState = APP_STATE_MQTT_CONNECTING;
            } else {
                Serial.println("Certificates not found");
                Serial.println("Will submit CSR to backend...");
                appState = APP_STATE_SUBMIT_CSR;
            }
            break;
            
        case APP_STATE_SUBMIT_CSR:
            {
                static bool csrSubmissionAttempted = false;
                static int csrRetryCount = 0;
                const int MAX_CSR_RETRIES = 3;
                
                if (!csrSubmissionAttempted) {
                    char deviceId[64] = {0};
                    char token[256] = {0};
                    
                    if (getProvisioningCredentials(deviceId, sizeof(deviceId), token, sizeof(token)) != 0) {
                        Serial.println("========================================");
                        Serial.println("FAILED to get provisioning credentials!");
                        Serial.println("========================================");
                        appState = APP_STATE_ERROR;
                        break;
                    }
                    
                    Serial.println("========================================");
                    Serial.println("Submitting CSR to backend server...");
                    Serial.println("========================================");
                    
                    int ret = CertificateManager::submitCSR(deviceId, token, BACKEND_URL);
                    csrSubmissionAttempted = true;
                    
                    if (ret == 0) {
                        Serial.println("========================================");
                        Serial.println("✓ CSR submitted successfully!");
                        Serial.println("✓ Certificates saved");
                        Serial.println("========================================");
                        csrRetryCount = 0;
                        appState = APP_STATE_MQTT_CONNECTING;
                    } else {
                        csrRetryCount++;
                        Serial.print("✗ CSR submission failed! Retry: ");
                        Serial.print(csrRetryCount);
                        Serial.print("/");
                        Serial.println(MAX_CSR_RETRIES);
                        
                        if (csrRetryCount >= MAX_CSR_RETRIES) {
                            Serial.println("Maximum retries reached. Moving to error state.");
                            appState = APP_STATE_ERROR;
                        } else {
                            csrSubmissionAttempted = false;
                            delay(5000);
                        }
                    }
                } else {
                    delay(1000);
                }
            }
            break;
            
        case APP_STATE_MQTT_CONNECTING:
            {
                static int mqttConnectRetries = 0;
                const int MAX_MQTT_RETRIES = 3;
                
                if (MQTTHandler::start(MQTT_BROKER, MQTT_PORT) == 0) {
                    Serial.println("MQTT handler started, waiting for connection...");
                    
                    int waitCount = 0;
                    while (!MQTTHandler::isConnected() && waitCount < 30) {
                        delay(1000);
                        waitCount++;
                        if (waitCount % 5 == 0) {
                            Serial.print("Waiting for MQTT connection... (");
                            Serial.print(waitCount);
                            Serial.println(" seconds)");
                        }
                    }
                    
                    if (MQTTHandler::isConnected()) {
                        Serial.println("✓ MQTT connected successfully!");
                        mqttConnectRetries = 0;
                        appState = APP_STATE_MQTT_CONNECTED;
                    } else {
                        Serial.println("MQTT connection timeout");
                        MQTTHandler::stop();
                        mqttConnectRetries++;
                        
                        if (mqttConnectRetries >= MAX_MQTT_RETRIES) {
                            Serial.print("MQTT connection failed after ");
                            Serial.print(MAX_MQTT_RETRIES);
                            Serial.println(" retries");
                            appState = APP_STATE_ERROR;
                        } else {
                            Serial.print("Retrying MQTT connection... (");
                            Serial.print(mqttConnectRetries);
                            Serial.print("/");
                            Serial.print(MAX_MQTT_RETRIES);
                            Serial.println(")");
                            delay(5000);
                        }
                    }
                } else {
                    Serial.println("ERROR: Failed to start MQTT handler");
                    mqttConnectRetries++;
                    
                    if (mqttConnectRetries >= MAX_MQTT_RETRIES) {
                        appState = APP_STATE_ERROR;
                    } else {
                        delay(5000);
                    }
                }
            }
            break;
            
        case APP_STATE_MQTT_CONNECTED:
            {
                static bool connectedMsgShown = false;
                
                if (!connectedMsgShown) {
                    Serial.println("========================================");
                    Serial.println("State: MQTT_CONNECTED");
                    Serial.println("========================================");
                    Serial.println("✓ Device provisioning complete!");
                    Serial.println("✓ mTLS MQTT connection established!");
                    Serial.println("✓ Device is fully operational!");
                    Serial.println("========================================");
                    connectedMsgShown = true;
                }
                
                // Check if still connected
                if (!MQTTHandler::isConnected()) {
                    Serial.println("MQTT connection lost, reconnecting...");
                    connectedMsgShown = false;
                    MQTTHandler::stop();
                    appState = APP_STATE_MQTT_CONNECTING;
                    break;
                }
                
                // Heartbeat log every 30 seconds
                static unsigned long lastHeartbeat = 0;
                if (now - lastHeartbeat > 30000) {
                    Serial.println("MQTT connection healthy - device operational");
                    lastHeartbeat = now;
                }
            }
            delay(1000);
            break;
            
        case APP_STATE_ERROR:
            Serial.println("ERROR: Application in error state");
            delay(10000);
            break;
    }
    
    // Small delay to prevent tight loop
    delay(100);
}

/**
 * @brief Get device ID and provisioning token from Preferences
 */
int getProvisioningCredentials(char *deviceId, size_t idLen, char *token, size_t tokenLen) {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        Serial.println("ERROR: Failed to open Preferences");
        return -1;
    }
    
    String deviceIdStr = prefs.getString("device_id", "");
    String tokenStr = prefs.getString("prov_token", "");
    
    prefs.end();
    
    if (deviceIdStr.length() == 0 || tokenStr.length() == 0) {
        Serial.println("ERROR: Provisioning credentials not found");
        return -1;
    }
    
    deviceIdStr.toCharArray(deviceId, idLen);
    tokenStr.toCharArray(token, tokenLen);
    
    Serial.print("✓ Credentials retrieved - Device ID: ");
    Serial.println(deviceId);
    
    return 0;
}

/**
 * @brief Clear all provisioning data (development mode)
 */
void clearProvisioningData() {
    Preferences prefs;
    if (prefs.begin("device_config", false)) {
        Serial.println("Clearing all provisioning data...");
        
        // Remove keys only if they exist (suppresses NOT_FOUND warnings)
        const char* keys[] = {
            "provisioned", "wifi_ssid", "wifi_pass", "device_id",
            "prov_token", "bearer_token", "device_cert", "ca_cert"
        };
        
        for (int i = 0; i < 8; i++) {
            if (prefs.isKey(keys[i])) {
                prefs.remove(keys[i]);
            }
        }
        
        prefs.end();
        
        Serial.println("✓ All provisioning data cleared");
        Serial.println("✓ Device will start in AP mode");
    } else {
        Serial.println("WARNING: Failed to open Preferences for clearing");
    }
}
