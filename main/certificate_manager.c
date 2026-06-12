/* Certificate Manager Implementation
 *
 * Handles CSR submission to backend and certificate storage/retrieval.
 */

#include <string.h>
#include <stdlib.h>
#include "certificate_manager.h"
#include "wifi_provisioning.h"
#include "device_keys.h"
#include "mtls_client_certs.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cert_mgr";

// NVS keys
#define NVS_NAMESPACE "device_config"
#define NVS_KEY_DEVICE_CERT "device_cert"
#define NVS_KEY_CA_CERT "ca_cert"

// Configuration from Kconfig
#define BACKEND_URL CONFIG_BACKEND_URL

// Buffer for HTTP response
static char *s_http_response_buffer = NULL;
static size_t s_http_response_len = 0;

static const char *extract_cert_pem_from_json(cJSON *node)
{
    if (node == NULL) {
        return NULL;
    }

    // Supports both formats:
    // 1) "certificate": "-----BEGIN CERTIFICATE-----..."
    // 2) "certificate": { "content": "-----BEGIN CERTIFICATE-----..." }
    if (cJSON_IsString(node)) {
        return node->valuestring;
    }

    if (cJSON_IsObject(node)) {
        cJSON *content = cJSON_GetObjectItem(node, "content");
        if (cJSON_IsString(content)) {
            return content->valuestring;
        }
    }

    return NULL;
}

/**
 * @brief HTTP event handler for esp_http_client
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            // Allocate buffer for response
            if (s_http_response_buffer == NULL) {
                s_http_response_len = esp_http_client_get_content_length(evt->client);
                if (s_http_response_len > 0 && s_http_response_len < 8192) {
                    s_http_response_buffer = malloc(s_http_response_len + 1);
                    if (s_http_response_buffer) {
                        s_http_response_buffer[0] = '\0';
                    }
                } else {
                    s_http_response_len = 8192;
                    s_http_response_buffer = malloc(s_http_response_len + 1);
                }
                if (s_http_response_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate response buffer");
                    return ESP_ERR_NO_MEM;
                }
            }
            // Append data to buffer
            size_t data_len = evt->data_len;
            size_t current_len = strlen(s_http_response_buffer);
            if (current_len + data_len < s_http_response_len) {
                memcpy(s_http_response_buffer + current_len, evt->data, data_len);
                s_http_response_buffer[current_len + data_len] = '\0';
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * @brief Save certificate to NVS
 */
static esp_err_t save_certificate_to_nvs(const char *key, const char *cert_pem)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, key, cert_pem);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving %s to NVS: %s", key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %s to NVS", key);
    }

    return err;
}

/**
 * @brief Submit CSR to backend
 * 
 * Sends CSR to backend for signing. The server will:
 * - Extract userId from provisioning_token
 * - Verify user and device associations
 * 
 * Payload includes:
 * - device_id: Device identifier
 * - csr: Certificate Signing Request
 * - provisioning_token: Token containing userId (server extracts it)
 */
esp_err_t certificate_manager_submit_csr(const char *device_id, const char *prov_token)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "CSR Submission to Backend");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Device ID: %s", device_id);
    ESP_LOGI(TAG, "Provisioning Token: %.*s... (length: %d)", 
             prov_token && strlen(prov_token) > 0 ? 20 : 0, 
             prov_token ? prov_token : "(null)", 
             prov_token ? strlen(prov_token) : 0);
    
    // Validate inputs
    if (device_id == NULL || strlen(device_id) == 0) {
        ESP_LOGE(TAG, "ERROR: device_id is NULL or empty!");
        return ESP_ERR_INVALID_ARG;
    }
    if (prov_token == NULL || strlen(prov_token) == 0) {
        ESP_LOGE(TAG, "ERROR: prov_token is NULL or empty!");
        return ESP_ERR_INVALID_ARG;
    }

    // Note: Server extracts userId from provisioning_token, so we don't need to send auth_token
    // The provisioning_token contains all necessary information for server validation

    // Build request URL (correct endpoint path: /api/v1/sign-csr)
    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/sign-csr", BACKEND_URL);
    ESP_LOGI(TAG, "Backend URL (from config): %s", BACKEND_URL);
    ESP_LOGI(TAG, "Full Endpoint URL: %s", url);
    
    // Check if BACKEND_URL is still the default placeholder
    if (strcmp(BACKEND_URL, "https://your-backend.com") == 0) {
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "ERROR: BACKEND_URL is still set to default placeholder!");
        ESP_LOGE(TAG, "Please configure BACKEND_URL in menuconfig:");
        ESP_LOGE(TAG, "  idf.py menuconfig -> Backend Configuration -> Backend URL");
        ESP_LOGE(TAG, "========================================");
        return ESP_ERR_INVALID_STATE;
    }

    // Build JSON request body with CSR, device_id, and provisioning_token
    // Note: Server extracts userId from provisioning_token and validates user-device association
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "csr", DEVICE_CSR_PEM);
    cJSON_AddStringToObject(root, "provisioning_token", prov_token);
    
    ESP_LOGI(TAG, "Payload includes: device_id, csr, provisioning_token");
    ESP_LOGI(TAG, "Server will extract userId from provisioning_token for validation");

    char *json_string = cJSON_Print(root);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON string");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Request body prepared (device_id + csr + provisioning_token)");
    ESP_LOGD(TAG, "Request body: %s", json_string);

    // Optional user bearer token (stored during provisioning).
    // If present, include it in Authorization header for backend-side user validation.
    char bearer_token[512] = {0};
    bool has_bearer_token = (wifi_provisioning_get_bearer_token(bearer_token, sizeof(bearer_token)) == ESP_OK &&
                             strlen(bearer_token) > 0);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 30000,
        .skip_cert_common_name_check = false,
    };

    // For HTTPS, we can use certificate bundle or skip verification for development
    // In production, you should verify the backend certificate
    #ifdef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    config.skip_cert_common_name_check = true;
    #endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(json_string);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    // Set headers
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (has_bearer_token) {
        char auth_header[600] = {0};
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, json_string, strlen(json_string));

    // Free response buffer if exists
    if (s_http_response_buffer) {
        free(s_http_response_buffer);
        s_http_response_buffer = NULL;
        s_http_response_len = 0;
    }

    // Log outgoing request
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "📤 OUTGOING HTTP REQUEST (Backend)");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Method: POST");
    ESP_LOGI(TAG, "URL: %s", url);
    ESP_LOGI(TAG, "Request Body (length: %d):", strlen(json_string));
    ESP_LOGD(TAG, "Request Body: %s", json_string);
    ESP_LOGI(TAG, "Headers:");
    ESP_LOGI(TAG, "  Content-Type: application/json");
    if (has_bearer_token) {
        ESP_LOGI(TAG, "  Authorization: Bearer <redacted>");
    } else {
        ESP_LOGI(TAG, "  Authorization: (not present; continuing with provisioning_token only)");
    }
    ESP_LOGI(TAG, "========================================");
    
    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "📥 INCOMING HTTP RESPONSE (Backend)");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Status Code: %d", status_code);

        if (status_code == 200 || status_code == 201) {
            // Parse response
            if (s_http_response_buffer) {
                ESP_LOGI(TAG, "Response Body (length: %d):", strlen(s_http_response_buffer));
                ESP_LOGD(TAG, "Response Body: %s", s_http_response_buffer);

                cJSON *response = cJSON_Parse(s_http_response_buffer);
                if (response) {
                    cJSON *cert_obj = cJSON_GetObjectItem(response, "certificate");
                    cJSON *ca_obj = cJSON_GetObjectItem(response, "ca_certificate");

                    if (cert_obj && ca_obj) {
                        const char *cert_pem = extract_cert_pem_from_json(cert_obj);
                        const char *ca_pem = extract_cert_pem_from_json(ca_obj);

                        if (cert_pem != NULL && ca_pem != NULL) {
                            // Save certificates to NVS
                            err = save_certificate_to_nvs(NVS_KEY_DEVICE_CERT, cert_pem);
                            if (err == ESP_OK) {
                                err = save_certificate_to_nvs(NVS_KEY_CA_CERT, ca_pem);
                            }

                            if (err == ESP_OK) {
                                ESP_LOGI(TAG, "✅ Successfully saved certificates");
                                ESP_LOGI(TAG, "========================================");
                            } else {
                                ESP_LOGE(TAG, "✗ Failed to save certificates: %s", esp_err_to_name(err));
                                ESP_LOGI(TAG, "========================================");
                            }
                        } else {
                            ESP_LOGE(TAG, "✗ Invalid certificate format in response");
                            ESP_LOGI(TAG, "========================================");
                            err = ESP_ERR_INVALID_RESPONSE;
                        }
                    } else {
                        ESP_LOGE(TAG, "✗ Missing certificate fields in response");
                        ESP_LOGI(TAG, "========================================");
                        err = ESP_ERR_INVALID_RESPONSE;
                    }
                    cJSON_Delete(response);
                } else {
                    ESP_LOGE(TAG, "✗ Failed to parse JSON response");
                    ESP_LOGI(TAG, "========================================");
                    err = ESP_ERR_INVALID_RESPONSE;
                }
            } else {
                ESP_LOGE(TAG, "✗ No response data received");
                ESP_LOGI(TAG, "========================================");
                err = ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            ESP_LOGE(TAG, "✗ HTTP request failed with status %d", status_code);
            if (s_http_response_buffer && strlen(s_http_response_buffer) > 0) {
                ESP_LOGE(TAG, "Error Response: %s", s_http_response_buffer);
            }
            ESP_LOGI(TAG, "========================================");
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "✗ HTTP POST request failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "========================================");
    }

    // Cleanup
    esp_http_client_cleanup(client);
    free(json_string);
    cJSON_Delete(root);

    if (s_http_response_buffer) {
        free(s_http_response_buffer);
        s_http_response_buffer = NULL;
        s_http_response_len = 0;
    }

    return err;
}

/**
 * @brief Check if certificates exist in NVS
 */
esp_err_t certificate_manager_erase_stored_certificates(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase certs: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_erase_key(nvs_handle, NVS_KEY_DEVICE_CERT);
    nvs_erase_key(nvs_handle, NVS_KEY_CA_CERT);
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erased device + CA certificates from NVS");
    }
    return err;
}

bool certificate_manager_has_certificates(void)
{
#if CONFIG_USE_EMBEDDED_MTLS_CERTS
    if (mtls_client_certs_available()) {
        return true;
    }
#endif

    nvs_handle_t nvs_handle;
    size_t required_size = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }

    esp_err_t err1 = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_CERT, NULL, &required_size);

    required_size = 0;
    esp_err_t err2 = nvs_get_str(nvs_handle, NVS_KEY_CA_CERT, NULL, &required_size);

    nvs_close(nvs_handle);

    return (err1 == ESP_OK && err2 == ESP_OK);
}

/**
 * @brief Load certificate from NVS
 */
static esp_err_t load_certificate_from_nvs(const char *key, char *cert_buffer, size_t buffer_size)
{
    nvs_handle_t nvs_handle;
    size_t required_size = buffer_size;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_str(nvs_handle, key, cert_buffer, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %s from NVS (%d bytes)", key, required_size);
    } else {
        ESP_LOGE(TAG, "Failed to load %s from NVS: %s", key, esp_err_to_name(err));
    }

    return err;
}

esp_err_t certificate_manager_load_device_cert(char *cert_buffer, size_t buffer_size)
{
#if CONFIG_USE_EMBEDDED_MTLS_CERTS
    if (mtls_client_certs_available()) {
        esp_err_t err = mtls_client_certs_load_device_cert(cert_buffer, buffer_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded device cert from embedded mtls_client/certs");
        }
        return err;
    }
#endif
    return load_certificate_from_nvs(NVS_KEY_DEVICE_CERT, cert_buffer, buffer_size);
}

esp_err_t certificate_manager_load_ca_cert(char *cert_buffer, size_t buffer_size)
{
#if CONFIG_USE_EMBEDDED_MTLS_CERTS
    if (mtls_client_certs_available()) {
        esp_err_t err = mtls_client_certs_load_ca_cert(cert_buffer, buffer_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loaded CA cert from embedded mtls_client/certs");
        }
        return err;
    }
#endif
    return load_certificate_from_nvs(NVS_KEY_CA_CERT, cert_buffer, buffer_size);
}

const char* certificate_manager_get_private_key(void)
{
#if CONFIG_USE_EMBEDDED_MTLS_CERTS
    const char *embedded_key = mtls_client_certs_get_private_key();
    if (embedded_key != NULL) {
        return embedded_key;
    }
#endif
    return DEVICE_PRIVATE_KEY_PEM;
}

