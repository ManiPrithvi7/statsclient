/* WiFi Provisioning Module Header - Arduino Version
 *
 * Handles WiFi Access Point mode for device provisioning.
 * Provides HTTP endpoints for WiFi scan and credential submission.
 */

#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

class WiFiProvisioning {
public:
    /**
     * @brief Start WiFi provisioning in AP mode with HTTP server
     * 
     * @param ssidPrefix AP SSID prefix
     * @param password AP password (empty string for open network)
     * @return 0 on success, -1 on error
     */
    static int start(const char* ssidPrefix, const char* password);
    
    /**
     * @brief Stop WiFi provisioning and HTTP server
     * 
     * @return 0 on success, -1 on error
     */
    static int stop();
    
    /**
     * @brief Check if device is already provisioned
     * 
     * @return true if WiFi credentials exist, false otherwise
     */
    static bool isProvisioned();
    
    /**
     * @brief Check if provisioning is currently active
     * 
     * @return true if AP mode is active, false otherwise
     */
    static bool isActive();
    
    /**
     * @brief Connect to WiFi using stored credentials
     * 
     * @return 0 on success, -1 on error
     */
    static int connectToWiFi();
    
    /**
     * @brief Get Bearer token from Preferences
     * 
     * @param token Output buffer for Bearer token
     * @param tokenLen Size of the token buffer
     * @return 0 on success, -1 if token doesn't exist
     */
    static int getBearerToken(char* token, size_t tokenLen);
    
    /**
     * @brief Clear provisioning credentials and return to AP mode
     * 
     * @return 0 on success, -1 on error
     */
    static int clearAndRestart();
    
    /**
     * @brief Get current WiFi connection status
     * 
     * @param ipAddr Output buffer for IP address (must be at least 16 bytes)
     * @return true if connected, false otherwise
     */
    static bool getStatus(char* ipAddr, size_t ipLen);

private:
    static AsyncWebServer* server;
    static bool provisioningActive;
    static bool wifiConnected;
    static char staIp[16];
    
    // WiFi scan cache
    struct WiFiNetworkInfo {
        String ssid;
        int rssi;
        int channel;
        bool secure;
    };
    
    static const int WIFI_SCAN_MAX_APS = 20;
    static WiFiNetworkInfo cachedNetworks[WIFI_SCAN_MAX_APS];
    static int cachedNetworkCount;
    static bool initialScanDone;
    
    // HTTP handlers
    static void handleRoot(AsyncWebServerRequest* request);
    static void handleScan(AsyncWebServerRequest* request);
    static void handleProvision(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
    static void handleStatus(AsyncWebServerRequest* request);
    static void handleOptions(AsyncWebServerRequest* request);
    
    // Helper functions
    static void setCORSHeaders(AsyncWebServerResponse* response, AsyncWebServerRequest* request);
    static int performWiFiScan();
    static int saveWiFiCredentials(const char* ssid, const char* password, 
                                   const char* deviceId, const char* provToken,
                                   const char* bearerToken);
    static void logRequest(AsyncWebServerRequest* request);
    static void logResponse(const char* method, const char* uri, int statusCode, const char* body);

    // WiFi event handler (registered via WiFi.onEvent)
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
};

#endif // WIFI_PROVISIONING_H
