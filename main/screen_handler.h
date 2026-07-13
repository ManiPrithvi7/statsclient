#ifndef SCREEN_HANDLER_H
#define SCREEN_HANDLER_H

#include "esp_err.h"

esp_err_t screen_handler_init(void);
void screen_handler_start(void);
void screen_handler_stop(void);
void screen_handler_on_mqtt(const char *topic, const char *payload, int len);

#endif /* SCREEN_HANDLER_H */
