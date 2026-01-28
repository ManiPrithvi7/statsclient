/* Internet Verification Implementation - Arduino Version
 *
 * Verifies internet connectivity by making an HTTPS request to a test endpoint.
 */

#include "InternetVerification.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define TEST_ENDPOINT_URL "https://mqtt-test-puf8.onrender.com/api/"

int InternetVerification::test() {
    Serial.println("========================================");
    Serial.println("Internet Connectivity Verification");
    Serial.println("========================================");
    Serial.print("Testing endpoint: ");
    Serial.println(TEST_ENDPOINT_URL);
    
    // Create HTTPS client
    WiFiClientSecure client;
    client.setInsecure(); // Skip certificate verification for development
    
    HTTPClient https;
    https.begin(client, TEST_ENDPOINT_URL);
    https.setTimeout(15000); // 15 second timeout
    
    Serial.println("Sending HTTPS GET request...");
    int httpCode = https.GET();
    
    Serial.print("HTTP Status Code: ");
    Serial.println(httpCode);
    
    if (httpCode == 200) {
        String response = https.getString();
        Serial.println("========================================");
        Serial.println("✓ INTERNET CONNECTIVITY VERIFIED!");
        Serial.println("✓ Provisioning flow 100% complete!");
        Serial.println("========================================");
        
        if (response.length() > 0) {
            Serial.print("Response from endpoint: ");
            Serial.println(response);
        }
        
        https.end();
        return 0;
    } else {
        Serial.print("ERROR: HTTP request failed with status: ");
        Serial.println(httpCode);
        String errorResponse = https.getString();
        if (errorResponse.length() > 0) {
            Serial.print("Error response: ");
            Serial.println(errorResponse);
        }
        https.end();
        return -1;
    }
}
