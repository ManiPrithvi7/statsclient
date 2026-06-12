#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_handler_init(const char *device_id);

esp_err_t ota_handler_start(void);

void ota_handler_on_mqtt_cmd(const char *topic, const char *payload, int len);

void ota_handler_on_mqtt_ack(const char *topic, const char *payload, int len);

void ota_handler_on_mqtt_connected(void);

esp_err_t ota_handler_run_pending_verify(void);

bool ota_handler_pending_verify_active(void);

void ota_handler_notify_wifi_mqtt_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_HANDLER_H */
