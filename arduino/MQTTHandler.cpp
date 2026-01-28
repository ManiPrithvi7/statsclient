/* MQTT Handler Implementation - Arduino Version
 *
 * Handles mTLS MQTT connection to the broker.
 */

#include "MQTTHandler.h"
#include "CertificateManager.h"
#include <Preferences.h>

// Static member initialization
WiFiClientSecure* MQTTHandler::secureClient = nullptr;
PubSubClient* MQTTHandler::mqttClient = nullptr;
bool MQTTHandler::connected = false;
char MQTTHandler::clientId[32] = {0};

int MQTTHandler::start(const char* broker, int port) {
    if (mqttClient != nullptr) {
        Serial.println("WARNING: MQTT handler already started");
        return 0;
    }
    
    Serial.println("========================================");
    Serial.println("Starting MQTT Handler with mTLS");
    Serial.println("========================================");
    
    // Check if certificates exist
    if (!CertificateManager::hasCertificates()) {
        Serial.println("ERROR: Certificates not found. Submit CSR first.");
        return -1;
    }
    
    // Load certificates
    char deviceCert[4096] = {0};
    char caCert[4096] = {0};
    
    if (CertificateManager::loadDeviceCert(deviceCert, sizeof(deviceCert)) != 0 ||
        CertificateManager::loadCACert(caCert, sizeof(caCert)) != 0) {
        Serial.println("ERROR: Failed to load certificates");
        return -1;
    }
    
    Serial.println("✓ Certificates loaded from Preferences");
    
    // Create secure client
    secureClient = new WiFiClientSecure();
    secureClient->setCACert(caCert);
    secureClient->setCertificate(deviceCert);
    secureClient->setPrivateKey(CertificateManager::getPrivateKey());
    
    // Create MQTT client
    mqttClient = new PubSubClient(*secureClient);
    mqttClient->setServer(broker, port);
    mqttClient->setCallback(mqttCallback);
    
    // Generate client ID
    snprintf(clientId, sizeof(clientId), "esp32_%lu", millis());
    
    Serial.print("MQTT Broker: ");
    Serial.print(broker);
    Serial.print(":");
    Serial.println(port);
    Serial.print("Client ID: ");
    Serial.println(clientId);
    
    // Connect to broker
    Serial.println("Connecting to MQTT broker...");
    if (mqttClient->connect(clientId)) {
        Serial.println("========================================");
        Serial.println("✓ mTLS handshake successful!");
        Serial.println("✓ Connected to MQTT broker");
        Serial.println("========================================");
        connected = true;
        return 0;
    } else {
        Serial.print("ERROR: MQTT connection failed, rc=");
        Serial.println(mqttClient->state());
        stop();
        return -1;
    }
}

void MQTTHandler::stop() {
    if (mqttClient) {
        if (connected) {
            mqttClient->disconnect();
        }
        delete mqttClient;
        mqttClient = nullptr;
    }
    
    if (secureClient) {
        delete secureClient;
        secureClient = nullptr;
    }
    
    connected = false;
    Serial.println("MQTT handler stopped");
}

bool MQTTHandler::isConnected() {
    if (mqttClient && connected) {
        return mqttClient->connected();
    }
    return false;
}

bool MQTTHandler::publish(const char* topic, const char* data, int dataLen, int qos) {
    if (!isConnected()) {
        return false;
    }
    
    return mqttClient->publish(topic, data, dataLen);
}

bool MQTTHandler::subscribe(const char* topic, int qos) {
    if (!isConnected()) {
        return false;
    }
    
    return mqttClient->subscribe(topic, qos);
}

void MQTTHandler::loop() {
    if (mqttClient) {
        mqttClient->loop();
        
        // Check connection status
        if (!mqttClient->connected() && connected) {
            Serial.println("MQTT connection lost");
            connected = false;
        }
    }
}

void MQTTHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("MQTT Message received [");
    Serial.print(topic);
    Serial.print("]: ");
    
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';
    Serial.println(message);
}

bool MQTTHandler::reconnect() {
    if (mqttClient && mqttClient->connect(clientId)) {
        Serial.println("MQTT reconnected");
        connected = true;
        return true;
    }
    return false;
}
