/* WiFi Provisioning Module Header
 *
 * Handles WiFi Access Point mode for device provisioning.
 * Provides HTTP endpoints for WiFi scan and credential submission.
 */

#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start WiFi provisioning in AP mode with HTTP server
 * 
 * Creates a WiFi Access Point and starts an HTTP server with provisioning endpoints:
 * - GET /scan - Returns list of available WiFi networks
 * - POST /provision - Accepts WiFi credentials and provisioning token
 * - GET /status - Returns current provisioning status
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_start(void);

/**
 * @brief Stop WiFi provisioning and HTTP server
 * 
 * Stops the HTTP server and switches WiFi to STA mode.
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_stop(void);

/**
 * @brief Check if device is already provisioned
 * 
 * @return true if WiFi credentials exist in NVS, false otherwise
 */
bool wifi_provisioning_is_provisioned(void);

/**
 * @brief Get current WiFi connection status
 * 
 * @param ip_addr Output buffer for IP address (must be at least 16 bytes)
 * @return true if connected, false otherwise
 */
bool wifi_provisioning_get_status(char *ip_addr, size_t ip_len);

/**
 * @brief Get Bearer token from NVS
 * 
 * Retrieves the Bearer token that was received in the Authorization header
 * during provisioning. This token is used for authenticated API calls to the server.
 * 
 * @param token Output buffer for Bearer token
 * @param token_len Size of the token buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if token doesn't exist
 */
esp_err_t wifi_provisioning_get_bearer_token(char *token, size_t token_len);

/**
 * @brief Clear provisioning credentials and return to AP mode
 * 
 * Clears all stored WiFi credentials from NVS and restarts the provisioning AP.
 * This is called when WiFi connection fails or internet verification fails,
 * allowing the device to wait for new credentials via HTTP POST.
 * 
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_clear_and_restart(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISIONING_H

