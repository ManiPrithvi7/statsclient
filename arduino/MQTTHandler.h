/* MQTT Handler Header - Arduino Version
 *
 * Handles mTLS MQTT connection to the broker.
 */

#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

class MQTTHandler {
public:
    /**
     * @brief Start MQTT handler with mTLS
     * 
     * @param broker MQTT broker hostname
     * @param port MQTT broker port (typically 8883 for mTLS)
     * @return 0 on success, -1 on error
     */
    static int start(const char* broker, int port);
    
    /**
     * @brief Stop MQTT handler
     */
    static void stop();
    
    /**
     * @brief Check if MQTT handler is connected
     * 
     * @return true if connected, false otherwise
     */
    static bool isConnected();
    
    /**
     * @brief Publish message to MQTT topic
     * 
     * @param topic Topic name
     * @param data Message data
     * @param dataLen Message length
     * @param qos Quality of Service (0, 1, or 2)
     * @return true on success, false otherwise
     */
    static bool publish(const char* topic, const char* data, int dataLen, int qos = 0);
    
    /**
     * @brief Subscribe to MQTT topic
     * 
     * @param topic Topic name
     * @param qos Quality of Service (0, 1, or 2)
     * @return true on success, false otherwise
     */
    static bool subscribe(const char* topic, int qos = 0);
    
    /**
     * @brief Process MQTT messages (call in loop)
     */
    static void loop();

private:
    static WiFiClientSecure* secureClient;
    static PubSubClient* mqttClient;
    static bool connected;
    static char clientId[32];
    
    static void mqttCallback(char* topic, byte* payload, unsigned int length);
    static bool reconnect();
};

#endif // MQTT_HANDLER_H
