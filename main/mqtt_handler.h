#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_cmd_callback_t)(const char *topic, const char *payload, int len);
typedef void (*mqtt_connected_callback_t)(void);

esp_err_t mqtt_handler_start(void);

void mqtt_handler_stop(void);

bool mqtt_handler_is_connected(void);

void mqtt_handler_set_device_id(const char *device_id);

void mqtt_handler_set_cmd_callback(mqtt_cmd_callback_t cb);

void mqtt_handler_set_ack_callback(mqtt_cmd_callback_t cb);

void mqtt_handler_set_connected_callback(mqtt_connected_callback_t cb);

esp_err_t mqtt_handler_publish(const char *topic, const char *data, int data_len, int qos);

esp_err_t mqtt_handler_subscribe(const char *topic, int qos);

esp_err_t mqtt_handler_publish_status_json(const char *json);

/** Clear retained payload on device cmd topic (stale ota_update after install). */
esp_err_t mqtt_handler_clear_retained_cmd(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HANDLER_H */
