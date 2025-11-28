/* WiFi Provisioning with mTLS MQTT - Main Application
 *
 * This application implements a complete provisioning flow:
 * 1. Boot → Check if device is provisioned
 * 2. If not provisioned → Start AP mode with HTTP server for provisioning
 * 3. After provisioning → Connect to WiFi
 * 4. Submit CSR to backend and receive certificates
 * 5. Connect to MQTT broker using mTLS
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_provisioning.h"
#include "certificate_manager.h"
#include "internet_verification.h"
#include "mqtt_handler.h"
#include "device_keys.h"

static const char *TAG = "main";

// NVS keys
#define NVS_NAMESPACE "device_config"
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_PROV_TOKEN "prov_token"

// Application states
typedef enum {
    APP_STATE_INIT,
    APP_STATE_CHECK_PROVISIONING,
    APP_STATE_AP_MODE,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_CHECK_CERTIFICATES,
    APP_STATE_SUBMIT_CSR,
    APP_STATE_MQTT_CONNECTING,
    APP_STATE_MQTT_CONNECTED,
    APP_STATE_ERROR
} app_state_t;

static app_state_t s_app_state = APP_STATE_INIT;

/**
 * @brief Get device ID and provisioning token from NVS
 */
static esp_err_t get_provisioning_credentials(char *device_id, size_t id_len,
                                              char *token, size_t token_len)
{
    nvs_handle_t nvs_handle;
    size_t required_size;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    required_size = id_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    required_size = token_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_PROV_TOKEN, token, &required_size);
    nvs_close(nvs_handle);

    return err;
}

/**
 * @brief WiFi event handler for STA connection
 */
static void wifi_sta_event_handler(void* arg, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi STA connected");
        s_app_state = APP_STATE_WIFI_CONNECTED;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_app_state = APP_STATE_WIFI_CONNECTED;
    }
}

/**
 * @brief Main application state machine task
 */
static void app_state_machine_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Application state machine started");

    while (1) {
        switch (s_app_state) {
        case APP_STATE_INIT:
            ESP_LOGI(TAG, "State: INIT");
            s_app_state = APP_STATE_CHECK_PROVISIONING;
            break;

        case APP_STATE_CHECK_PROVISIONING:
            ESP_LOGI(TAG, "State: CHECK_PROVISIONING");
            if (wifi_provisioning_is_provisioned()) {
                ESP_LOGI(TAG, "Device is provisioned, connecting to WiFi...");
                s_app_state = APP_STATE_WIFI_CONNECTING;
            } else {
                ESP_LOGI(TAG, "Device not provisioned, starting AP mode...");
                s_app_state = APP_STATE_AP_MODE;
            }
            break;

        case APP_STATE_AP_MODE:
            ESP_LOGI(TAG, "State: AP_MODE");
            {
                // Check if provisioning is already active
                // If not, start it (handles both initial start and restart after failure)
                if (!wifi_provisioning_is_provisioned()) {
                    // Try to start provisioning if not already active
                    // wifi_provisioning_start() checks internally if already active
                    esp_err_t ret = wifi_provisioning_start();
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(ret));
                        ESP_LOGE(TAG, "Retrying in 5 seconds...");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    } else {
                        ESP_LOGI(TAG, "Provisioning AP active. Waiting for credentials via HTTP POST /provision...");
                    }
                } else {
                    // Device is provisioned, move to connecting state
                    ESP_LOGI(TAG, "Device is provisioned, moving to WiFi connecting state");
                    s_app_state = APP_STATE_WIFI_CONNECTING;
                    break;
                }
                
                // Wait in AP mode for credentials
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            break;

        case APP_STATE_WIFI_CONNECTING:
            ESP_LOGI(TAG, "State: WIFI_CONNECTING");
            {
                static bool connection_attempted = false;
                
                if (!connection_attempted) {
                    // Read WiFi credentials from NVS and connect
                    nvs_handle_t nvs_handle;
                    if (nvs_open("device_config", NVS_READONLY, &nvs_handle) == ESP_OK) {
                        char ssid[33] = {0};
                        char password[65] = {0};
                        size_t required_size;
                        
                        required_size = sizeof(ssid);
                        if (nvs_get_str(nvs_handle, "wifi_ssid", ssid, &required_size) == ESP_OK) {
                            required_size = sizeof(password);
                            nvs_get_str(nvs_handle, "wifi_password", password, &required_size);
                            
                            // Configure and connect to WiFi
                            wifi_config_t wifi_config = {0};
                            strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
                            strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
                            
                            ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
                            esp_wifi_set_mode(WIFI_MODE_STA);
                            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                            esp_wifi_start();
                            esp_wifi_connect();
                            
                            connection_attempted = true;
                        }
                        nvs_close(nvs_handle);
                    }
                }
                
                // Wait for connection event (handled by wifi_sta_event_handler)
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            break;

        case APP_STATE_WIFI_CONNECTED:
            ESP_LOGI(TAG, "State: WIFI_CONNECTED");
            {
                static bool verification_done = false;
                static int verification_retries = 0;
                const int MAX_VERIFICATION_RETRIES = 2; // Try 2 times before giving up
                
                // Reset verification state if we're not provisioned (means we returned to AP mode)
                if (!wifi_provisioning_is_provisioned()) {
                    verification_done = false;
                    verification_retries = 0;
                    s_app_state = APP_STATE_AP_MODE;
                    break;
                }
                
                if (!verification_done) {
                    // Verify internet connectivity after WiFi connection
                    ESP_LOGI(TAG, "WiFi connected - verifying internet access...");
                    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds for network to stabilize
                    
                    esp_err_t ret = internet_verification_test();
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "✓ Internet connectivity verified!");
                        ESP_LOGI(TAG, "✓ Provisioning flow 100%% complete!");
                        verification_done = true;
                        verification_retries = 0; // Reset retry counter
                    } else {
                        verification_retries++;
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "✗ Internet verification failed!");
                        ESP_LOGE(TAG, "✗ Retry attempt: %d/%d", verification_retries, MAX_VERIFICATION_RETRIES);
                        ESP_LOGE(TAG, "========================================");
                        
                        if (verification_retries >= MAX_VERIFICATION_RETRIES) {
                            ESP_LOGE(TAG, "Maximum retries reached. Credentials may be incorrect.");
                            ESP_LOGE(TAG, "WiFi may be connected but has no internet access.");
                            ESP_LOGI(TAG, "Clearing credentials and returning to AP mode...");
                            ESP_LOGI(TAG, "Please send new credentials via HTTP POST /provision");
                            
                            // Clear credentials and return to AP mode
                            wifi_provisioning_clear_and_restart();
                            
                            // Reset state machine to AP mode
                            verification_done = false;
                            verification_retries = 0;
                            s_app_state = APP_STATE_AP_MODE;
                            break;
                        } else {
                            ESP_LOGW(TAG, "Retrying internet verification in 5 seconds...");
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                        break;
                    }
                }
                
                if (verification_done) {
                    s_app_state = APP_STATE_CHECK_CERTIFICATES;
                }
            }
            break;

        case APP_STATE_CHECK_CERTIFICATES:
            ESP_LOGI(TAG, "State: CHECK_CERTIFICATES");
            if (certificate_manager_has_certificates()) {
                ESP_LOGI(TAG, "✓ Certificates found in NVS");
                ESP_LOGI(TAG, "Proceeding to MQTT connection...");
                s_app_state = APP_STATE_MQTT_CONNECTING;
            } else {
                ESP_LOGI(TAG, "Certificates not found, submitting CSR...");
                s_app_state = APP_STATE_SUBMIT_CSR;
            }
            break;

        case APP_STATE_SUBMIT_CSR:
            ESP_LOGI(TAG, "State: SUBMIT_CSR");
            {
                char device_id[64] = {0};
                char token[256] = {0};

                esp_err_t ret = get_provisioning_credentials(device_id, sizeof(device_id),
                                                             token, sizeof(token));
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to get provisioning credentials: %s", esp_err_to_name(ret));
                    s_app_state = APP_STATE_ERROR;
                    break;
                }

                ret = certificate_manager_submit_csr(device_id, token);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "CSR submitted successfully, certificates saved");
                    s_app_state = APP_STATE_MQTT_CONNECTING;
                } else {
                    ESP_LOGE(TAG, "Failed to submit CSR: %s", esp_err_to_name(ret));
                    // Retry after delay
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            }
            break;

        case APP_STATE_MQTT_CONNECTING:
            ESP_LOGI(TAG, "State: MQTT_CONNECTING");
            {
                static int mqtt_connect_retries = 0;
                const int MAX_MQTT_RETRIES = 3;
                
                esp_err_t ret = mqtt_handler_start();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "MQTT handler started, waiting for connection...");
                    
                    // Wait for connection with timeout
                    int wait_count = 0;
                    while (!mqtt_handler_is_connected() && wait_count < 30) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        wait_count++;
                        if (wait_count % 5 == 0) {
                            ESP_LOGI(TAG, "Waiting for MQTT connection... (%d seconds)", wait_count);
                        }
                    }
                    
                    if (mqtt_handler_is_connected()) {
                        ESP_LOGI(TAG, "✓ MQTT connected successfully!");
                        mqtt_connect_retries = 0;
                        s_app_state = APP_STATE_MQTT_CONNECTED;
                    } else {
                        ESP_LOGW(TAG, "MQTT connection timeout");
                        mqtt_handler_stop();
                        mqtt_connect_retries++;
                        
                        if (mqtt_connect_retries >= MAX_MQTT_RETRIES) {
                            ESP_LOGE(TAG, "MQTT connection failed after %d retries", MAX_MQTT_RETRIES);
                            s_app_state = APP_STATE_ERROR;
                        } else {
                            ESP_LOGI(TAG, "Retrying MQTT connection... (%d/%d)", mqtt_connect_retries, MAX_MQTT_RETRIES);
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to start MQTT handler: %s", esp_err_to_name(ret));
                    mqtt_connect_retries++;
                    
                    if (mqtt_connect_retries >= MAX_MQTT_RETRIES) {
                        s_app_state = APP_STATE_ERROR;
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                }
            }
            break;

        case APP_STATE_MQTT_CONNECTED:
            {
                static bool connected_msg_shown = false;
                
                if (!connected_msg_shown) {
                    ESP_LOGI(TAG, "========================================");
                    ESP_LOGI(TAG, "State: MQTT_CONNECTED");
                    ESP_LOGI(TAG, "========================================");
                    ESP_LOGI(TAG, "✓ Device provisioning complete!");
                    ESP_LOGI(TAG, "✓ mTLS MQTT connection established!");
                    ESP_LOGI(TAG, "✓ Device is fully operational!");
                    ESP_LOGI(TAG, "========================================");
                    connected_msg_shown = true;
                }
                
                // Check if still connected
                if (!mqtt_handler_is_connected()) {
                    ESP_LOGW(TAG, "MQTT connection lost, reconnecting...");
                    connected_msg_shown = false;
                    mqtt_handler_stop();
                    s_app_state = APP_STATE_MQTT_CONNECTING;
                    break;
                }
                
                // Application is fully operational - can publish/subscribe here
                // For now, just heartbeat log every 30 seconds
                static int heartbeat_counter = 0;
                heartbeat_counter++;
                if (heartbeat_counter >= 30) {
                    ESP_LOGI(TAG, "MQTT connection healthy - device operational");
                    heartbeat_counter = 0;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        case APP_STATE_ERROR:
            ESP_LOGE(TAG, "State: ERROR - Application in error state");
            // Could implement error recovery here
            vTaskDelay(pdMS_TO_TICKS(10000));
            break;

        default:
            ESP_LOGW(TAG, "Unknown state: %d", s_app_state);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to prevent tight loop
    }
}

/**
 * @brief Main application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== WiFi Provisioning with mTLS MQTT ===");
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // DEVELOPMENT MODE: Clear all provisioning data on every boot
    // This ensures a fresh start for development/testing
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "DEVELOPMENT MODE: Clearing provisioning");
    ESP_LOGI(TAG, "========================================");
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        ESP_LOGI(TAG, "Clearing all provisioning data...");
        
        // Erase all provisioning-related keys
        nvs_erase_key(nvs_handle, "provisioned");  // Provisioning status flag
        nvs_erase_key(nvs_handle, "wifi_ssid");     // WiFi SSID
        nvs_erase_key(nvs_handle, "wifi_pass");     // WiFi password
        nvs_erase_key(nvs_handle, NVS_KEY_DEVICE_ID);  // Device ID
        nvs_erase_key(nvs_handle, NVS_KEY_PROV_TOKEN); // Provisioning token
        nvs_erase_key(nvs_handle, "bearer_token");  // Bearer token
        nvs_erase_key(nvs_handle, "device_cert");   // Device certificate
        nvs_erase_key(nvs_handle, "ca_cert");       // CA certificate
        
        // Commit changes
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        ESP_LOGI(TAG, "✓ All provisioning data cleared");
        ESP_LOGI(TAG, "✓ Device will start in AP mode");
        ESP_LOGI(TAG, "========================================");
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for clearing (may be first boot)");
    }

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "Network interface initialized");

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Event loop created");

    // Register WiFi event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_sta_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_sta_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_LOGI(TAG, "Event handlers registered");

    // Start state machine task
    xTaskCreate(app_state_machine_task, "app_state_machine", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "State machine task started");

    ESP_LOGI(TAG, "Application initialization complete");
}
