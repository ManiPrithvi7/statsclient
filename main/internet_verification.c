/* Internet Verification Implementation
 *
 * Verifies internet connectivity by making an HTTPS request to a test endpoint.
 * This confirms that the provisioning flow is 100% complete.
 */

#include <string.h>
#include <stdlib.h>
#include "internet_verification.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"

static const char *TAG = "inet_verify";

// Test endpoint URL
#define TEST_ENDPOINT_URL "https://mqtt-test-puf8.onrender.com/api/"

// Buffer for HTTP response
static char *s_response_buffer = NULL;
static size_t s_response_len = 0;

/**
 * @brief HTTP event handler for internet verification
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "Connected to test endpoint");
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
            if (s_response_buffer == NULL) {
                s_response_len = esp_http_client_get_content_length(evt->client);
                if (s_response_len > 0 && s_response_len < 4096) {
                    s_response_buffer = malloc(s_response_len + 1);
                    if (s_response_buffer) {
                        s_response_buffer[0] = '\0';
                    }
                } else {
                    s_response_len = 4096;
                    s_response_buffer = malloc(s_response_len + 1);
                }
                if (s_response_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate response buffer");
                    return ESP_ERR_NO_MEM;
                }
            }
            // Append data to buffer
            size_t data_len = evt->data_len;
            size_t current_len = strlen(s_response_buffer);
            if (current_len + data_len < s_response_len) {
                memcpy(s_response_buffer + current_len, evt->data, data_len);
                s_response_buffer[current_len + data_len] = '\0';
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
 * @brief Verify internet connectivity by accessing test endpoint
 */
esp_err_t internet_verification_test(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Internet Connectivity Verification");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Testing endpoint: %s", TEST_ENDPOINT_URL);
    
    // Reset response buffer
    if (s_response_buffer) {
        free(s_response_buffer);
        s_response_buffer = NULL;
    }
    s_response_len = 0;
    
    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = TEST_ENDPOINT_URL,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,  // 15 second timeout
        .skip_cert_common_name_check = true,  // Skip cert check for development
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    // Perform GET request
    ESP_LOGI(TAG, "Sending HTTPS GET request...");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "HTTP Status Code: %d", status_code);
        ESP_LOGI(TAG, "Content Length: %d", content_length);
        
        if (status_code == 200) {
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "✓ INTERNET CONNECTIVITY VERIFIED!");
            ESP_LOGI(TAG, "✓ Provisioning flow 100%% complete!");
            ESP_LOGI(TAG, "========================================");
            
            if (s_response_buffer && strlen(s_response_buffer) > 0) {
                ESP_LOGI(TAG, "Response from endpoint:");
                ESP_LOGI(TAG, "%s", s_response_buffer);
            } else {
                ESP_LOGW(TAG, "No response body received");
            }
            
            esp_http_client_cleanup(client);
            if (s_response_buffer) {
                free(s_response_buffer);
                s_response_buffer = NULL;
            }
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
            if (s_response_buffer && strlen(s_response_buffer) > 0) {
                ESP_LOGE(TAG, "Error response: %s", s_response_buffer);
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    if (s_response_buffer) {
        free(s_response_buffer);
        s_response_buffer = NULL;
    }
    
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "✗ Internet connectivity verification failed");
    ESP_LOGE(TAG, "========================================");
    return ESP_FAIL;
}

