/* Certificate Manager Header
 *
 * Handles CSR submission to backend and certificate storage/retrieval.
 */

#ifndef CERTIFICATE_MANAGER_H
#define CERTIFICATE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Submit CSR to backend and retrieve signed certificates
 * 
 * Makes an authenticated HTTPS POST request to the backend's /sign-csr endpoint
 * with the device's CSR and provisioning token. On success, saves the returned
 * device certificate and CA certificate to NVS.
 * 
 * @param device_id Device identifier
 * @param token Provisioning token for authentication
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t certificate_manager_submit_csr(const char *device_id, const char *token);

/**
 * @brief Check if certificates are stored in NVS
 * 
 * @return true if both device certificate and CA certificate exist, false otherwise
 */
bool certificate_manager_has_certificates(void);

/**
 * @brief Load device certificate from NVS
 * 
 * @param cert_buffer Output buffer for certificate (PEM format)
 * @param buffer_size Size of the buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t certificate_manager_load_device_cert(char *cert_buffer, size_t buffer_size);

/**
 * @brief Load CA certificate from NVS
 * 
 * @param cert_buffer Output buffer for CA certificate (PEM format)
 * @param buffer_size Size of the buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t certificate_manager_load_ca_cert(char *cert_buffer, size_t buffer_size);

/**
 * @brief Get device private key (from device_keys.h)
 * 
 * @return Pointer to private key string (PEM format)
 */
const char* certificate_manager_get_private_key(void);

#ifdef __cplusplus
}
#endif

#endif // CERTIFICATE_MANAGER_H

