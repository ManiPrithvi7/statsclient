/* MQTT Client Header
 *
 * Handles mTLS MQTT connection to the broker.
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start MQTT client with mTLS
 * 
 * Loads certificates from NVS and private key from device_keys.h,
 * then connects to the MQTT broker using mTLS authentication.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_start(void);

/**
 * @brief Stop MQTT client
 * 
 * Disconnects from the broker and cleans up resources.
 */
void mqtt_client_stop(void);

/**
 * @brief Check if MQTT client is connected
 * 
 * @return true if connected, false otherwise
 */
bool mqtt_client_is_connected(void);

/**
 * @brief Publish message to MQTT topic
 * 
 * @param topic Topic name
 * @param data Message data
 * @param data_len Message length
 * @param qos Quality of Service (0, 1, or 2)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_publish(const char *topic, const char *data, int data_len, int qos);

/**
 * @brief Subscribe to MQTT topic
 * 
 * @param topic Topic name
 * @param qos Quality of Service (0, 1, or 2)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_client_subscribe(const char *topic, int qos);

#ifdef __cplusplus
}
#endif

#endif // MQTT_CLIENT_H

