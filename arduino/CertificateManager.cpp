/* Certificate Manager Implementation - Arduino Version
 *
 * Handles CSR submission to backend and certificate storage/retrieval.
 */

#include "CertificateManager.h"
#include "DeviceKeys.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>

int CertificateManager::submitCSR(const char* deviceId, const char* token, const char* backendUrl) {
    Serial.println("========================================");
    Serial.println("CSR Submission to Backend");
    Serial.println("========================================");
    Serial.print("Device ID: ");
    Serial.println(deviceId);
    
    if (!deviceId || strlen(deviceId) == 0) {
        Serial.println("ERROR: device_id is NULL or empty!");
        return -1;
    }
    if (!token || strlen(token) == 0) {
        Serial.println("ERROR: token is NULL or empty!");
        return -1;
    }
    
    // Build request URL
    String url = String(backendUrl) + "/api/v1/sign-csr";
    Serial.print("Full Endpoint URL: ");
    Serial.println(url);
    
    // Build JSON request body
    DynamicJsonDocument doc(2048);
    doc["device_id"] = deviceId;
    doc["csr"] = DEVICE_CSR_PEM;
    doc["provisioning_token"] = token;
    
    String jsonBody;
    serializeJson(doc, jsonBody);
    
    Serial.println("Request body prepared (device_id + csr + provisioning_token)");
    
    // Create HTTPS client
    WiFiClientSecure client;
    client.setInsecure(); // Skip certificate verification for development
    
    HTTPClient https;
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");
    https.setTimeout(30000); // 30 second timeout
    
    Serial.println("========================================");
    Serial.println("📤 OUTGOING HTTP REQUEST (Backend)");
    Serial.println("========================================");
    Serial.println("Method: POST");
    Serial.print("URL: ");
    Serial.println(url);
    Serial.print("Request Body Length: ");
    Serial.println(jsonBody.length());
    
    // Perform POST request
    int httpCode = https.POST(jsonBody);
    
    Serial.print("HTTP Status Code: ");
    Serial.println(httpCode);
    
    if (httpCode == 200) {
        String response = https.getString();
        Serial.print("Response Length: ");
        Serial.println(response.length());
        Serial.print("Response: ");
        Serial.println(response);
        
        // Parse response
        DynamicJsonDocument responseDoc(4096);
        DeserializationError error = deserializeJson(responseDoc, response);
        
        if (error) {
            Serial.print("ERROR: Failed to parse JSON response: ");
            Serial.println(error.c_str());
            https.end();
            return -1;
        }
        
        // Extract certificates
        if (responseDoc.containsKey("device_cert") && responseDoc.containsKey("ca_cert")) {
            String deviceCert = responseDoc["device_cert"].as<String>();
            String caCert = responseDoc["ca_cert"].as<String>();
            
            // Save certificates
            if (saveCertificate("device_cert", deviceCert.c_str()) != 0 ||
                saveCertificate("ca_cert", caCert.c_str()) != 0) {
                Serial.println("ERROR: Failed to save certificates");
                https.end();
                return -1;
            }
            
            Serial.println("========================================");
            Serial.println("✓ CSR submitted successfully!");
            Serial.println("✓ Certificates saved to Preferences");
            Serial.println("========================================");
            https.end();
            return 0;
        } else {
            Serial.println("ERROR: Response missing certificates");
            https.end();
            return -1;
        }
    } else {
        String errorResponse = https.getString();
        Serial.print("ERROR: HTTP request failed with status: ");
        Serial.println(httpCode);
        Serial.print("Error response: ");
        Serial.println(errorResponse);
        https.end();
        return -1;
    }
}

bool CertificateManager::hasCertificates() {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        return false;
    }
    
    String deviceCert = prefs.getString("device_cert", "");
    String caCert = prefs.getString("ca_cert", "");
    prefs.end();
    
    return (deviceCert.length() > 0 && caCert.length() > 0);
}

int CertificateManager::loadDeviceCert(char* certBuffer, size_t bufferSize) {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        return -1;
    }
    
    String cert = prefs.getString("device_cert", "");
    prefs.end();
    
    if (cert.length() == 0) {
        return -1;
    }
    
    cert.toCharArray(certBuffer, bufferSize);
    return 0;
}

int CertificateManager::loadCACert(char* certBuffer, size_t bufferSize) {
    Preferences prefs;
    if (!prefs.begin("device_config", true)) {
        return -1;
    }
    
    String cert = prefs.getString("ca_cert", "");
    prefs.end();
    
    if (cert.length() == 0) {
        return -1;
    }
    
    cert.toCharArray(certBuffer, bufferSize);
    return 0;
}

const char* CertificateManager::getPrivateKey() {
    return DEVICE_PRIVATE_KEY_PEM;
}

int CertificateManager::saveCertificate(const char* key, const char* certPem) {
    Preferences prefs;
    if (!prefs.begin("device_config", false)) {
        Serial.print("ERROR: Failed to open Preferences for ");
        Serial.println(key);
        return -1;
    }
    
    prefs.putString(key, certPem);
    prefs.end();
    
    Serial.print("Saved ");
    Serial.print(key);
    Serial.println(" to Preferences");
    return 0;
}
