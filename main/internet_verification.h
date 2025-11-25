#ifndef INTERNET_VERIFICATION_H
#define INTERNET_VERIFICATION_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Verify internet connectivity by accessing test endpoint
 * 
 * Makes an HTTPS GET request to https://mqtt-test-puf8.onrender.com/api/
 * to verify the device has internet access after WiFi connection.
 * 
 * @return ESP_OK if internet access is confirmed, ESP_FAIL otherwise
 */
esp_err_t internet_verification_test(void);

#ifdef __cplusplus
}
#endif

#endif // INTERNET_VERIFICATION_H

