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
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"

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
 * @brief Check if required provisioning credentials already exist in NVS
 *
 * This allows the app to proceed without waiting for another /provision HTTP call
 * when credentials were already stored by the captive portal.
 */
static bool has_local_provisioning_credentials(void)
{
    nvs_handle_t nvs_handle;
    size_t required = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }

    bool has_ssid = (nvs_get_str(nvs_handle, NVS_KEY_WIFI_SSID, NULL, &required) == ESP_OK);
    required = 0;
    bool has_pass = (nvs_get_str(nvs_handle, NVS_KEY_WIFI_PASS, NULL, &required) == ESP_OK);
    required = 0;
    bool has_device_id = (nvs_get_str(nvs_handle, NVS_KEY_DEVICE_ID, NULL, &required) == ESP_OK);
    required = 0;
    bool has_prov_token = (nvs_get_str(nvs_handle, NVS_KEY_PROV_TOKEN, NULL, &required) == ESP_OK);

    nvs_close(nvs_handle);

    return has_ssid && has_pass && has_device_id && has_prov_token;
}

/**
 * @brief Get device ID and provisioning token from NVS
 */
static esp_err_t get_provisioning_credentials(char *device_id, size_t id_len,
                                              char *token, size_t token_len)
{
    nvs_handle_t nvs_handle;
    size_t required_size;

    ESP_LOGI(TAG, "Opening NVS namespace: %s", NVS_NAMESPACE);
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Read device_id
    required_size = id_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id, &required_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read %s from NVS: %s", NVS_KEY_DEVICE_ID, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Read device_id from NVS: %s (length: %d)", device_id, strlen(device_id));

    // Read provisioning token
    // First query required size so we can provide clear diagnostics for long JWTs
    required_size = 0;
    err = nvs_get_str(nvs_handle, NVS_KEY_PROV_TOKEN, NULL, &required_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query size of %s from NVS: %s", NVS_KEY_PROV_TOKEN, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    if (required_size > token_len) {
        ESP_LOGE(TAG, "Provisioning token buffer too small: need %d bytes, have %d bytes",
                 (int)required_size, (int)token_len);
        nvs_close(nvs_handle);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_PROV_TOKEN, token, &required_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read %s from NVS: %s", NVS_KEY_PROV_TOKEN, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    ESP_LOGI(TAG, "Read prov_token from NVS: %.*s... (length: %d)", 
             20, token, strlen(token));
    
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
    
    // Log state transitions
    static app_state_t last_state = APP_STATE_INIT;
    if (s_app_state != last_state) {
        ESP_LOGI(TAG, ">>> STATE TRANSITION: %d -> %d", last_state, s_app_state);
        last_state = s_app_state;
    }

    while (1) {
        // Log state transitions
        if (s_app_state != last_state) {
            ESP_LOGI(TAG, ">>> STATE TRANSITION: %d -> %d", last_state, s_app_state);
            last_state = s_app_state;
        }
        
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
                // If credentials already exist, do not wait for another incoming /provision call.
                if (has_local_provisioning_credentials()) {
                    ESP_LOGI(TAG, "✅ STEP: Found local provisioning credentials in NVS");
                    ESP_LOGI(TAG, "✅ STEP: Skipping AP wait and moving to WiFi connect");
                    s_app_state = APP_STATE_WIFI_CONNECTING;
                    break;
                }

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
                static bool wifi_sta_inited = false;

                if (!connection_attempted) {
                    // When we skip AP mode (credential reuse), WiFi was never initialized.
                    // Ensure STA netif exists and esp_wifi_init() has been called before connecting.
                    if (!wifi_sta_inited) {
                        esp_netif_create_default_wifi_sta();
                        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                        esp_err_t err = esp_wifi_init(&cfg);
                        if (err != ESP_OK) {
                            ESP_LOGW(TAG, "esp_wifi_init: %s (continuing in case already inited)", esp_err_to_name(err));
                        }
                        wifi_sta_inited = true;
                        ESP_LOGI(TAG, "✅ STEP: WiFi driver initialized for STA");
                    }

                    // Read WiFi credentials from NVS and connect
                    nvs_handle_t nvs_handle;
                    if (nvs_open("device_config", NVS_READONLY, &nvs_handle) == ESP_OK) {
                        char ssid[33] = {0};
                        char password[65] = {0};
                        size_t required_size;

                        required_size = sizeof(ssid);
                        if (nvs_get_str(nvs_handle, "wifi_ssid", ssid, &required_size) == ESP_OK) {
                            required_size = sizeof(password);
                            nvs_get_str(nvs_handle, "wifi_pass", password, &required_size);

                            // Trim leading/trailing whitespace (can prevent connection)
                            size_t n = strlen(ssid);
                            while (n > 0 && (ssid[n - 1] == ' ' || ssid[n - 1] == '\t')) { ssid[--n] = '\0'; }
                            for (char *p = ssid; *p == ' ' || *p == '\t'; p++) {
                                n = strlen(p + 1);
                                memmove(ssid, p + 1, n + 1);
                            }
                            n = strlen(password);
                            while (n > 0 && (password[n - 1] == ' ' || password[n - 1] == '\t')) { password[--n] = '\0'; }
                            for (char *p = password; *p == ' ' || *p == '\t'; p++) {
                                n = strlen(p + 1);
                                memmove(password, p + 1, n + 1);
                            }

                            wifi_config_t wifi_config = {0};
                            strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
                            strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

                            ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
                            ESP_LOGI(TAG, "✅ STEP: WiFi credentials loaded; attempting STA connection");

                            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
                                vTaskDelay(pdMS_TO_TICKS(2000));
                                nvs_close(nvs_handle);
                                break;
                            }
                            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
                                vTaskDelay(pdMS_TO_TICKS(2000));
                                nvs_close(nvs_handle);
                                break;
                            }
                            err = esp_wifi_start();
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
                                vTaskDelay(pdMS_TO_TICKS(2000));
                                nvs_close(nvs_handle);
                                break;
                            }
                            err = esp_wifi_connect();
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
                                vTaskDelay(pdMS_TO_TICKS(2000));
                                nvs_close(nvs_handle);
                                break;
                            }

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
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "State: WIFI_CONNECTED");
            ESP_LOGI(TAG, "========================================");
            {
                static bool verification_done = false;
                static int verification_retries = 0;
                const int MAX_VERIFICATION_RETRIES = 2; // Try 2 times before giving up
                
                // Reset verification state if we're not provisioned (means we returned to AP mode)
                if (!wifi_provisioning_is_provisioned()) {
                    ESP_LOGW(TAG, "Device not provisioned, resetting to AP mode");
                    verification_done = false;
                    verification_retries = 0;
                    s_app_state = APP_STATE_AP_MODE;
                    break;
                }
                
                if (!verification_done) {
                    // Verify internet connectivity after WiFi connection
                    ESP_LOGI(TAG, "WiFi connected - verifying internet access...");
                    ESP_LOGI(TAG, "Waiting 2 seconds for network to stabilize...");
                    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds for network to stabilize
                    
                    ESP_LOGI(TAG, "Calling internet_verification_test()...");
                    esp_err_t ret = internet_verification_test();
                    ESP_LOGI(TAG, "Internet verification returned: %s", esp_err_to_name(ret));
                    
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "========================================");
                        ESP_LOGI(TAG, "✓ Internet connectivity verified!");
                        ESP_LOGI(TAG, "========================================");
                        ESP_LOGI(TAG, "✅ STEP: Internet verified; proceeding to certificate check");
                        verification_done = true;
                        verification_retries = 0; // Reset retry counter
                    } else {
                        verification_retries++;
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "✗ Internet verification failed!");
                        ESP_LOGE(TAG, "✗ Error: %s", esp_err_to_name(ret));
                        ESP_LOGE(TAG, "✗ Retry attempt: %d/%d", verification_retries, MAX_VERIFICATION_RETRIES);
                        ESP_LOGE(TAG, "========================================");
                        
                        if (verification_retries >= MAX_VERIFICATION_RETRIES) {
                            ESP_LOGE(TAG, "Maximum retries reached. Credentials may be incorrect.");
                            ESP_LOGE(TAG, "WiFi may be connected but has no internet access.");
                            ESP_LOGI(TAG, "NOTE: Proceeding to certificate check anyway...");
                            ESP_LOGI(TAG, "CSR submission will be attempted even without internet verification.");
                            
                            // Instead of clearing credentials, proceed to certificate check
                            // This allows CSR submission to be attempted
                            verification_done = true; // Mark as done to proceed
                            verification_retries = 0;
                        } else {
                            ESP_LOGW(TAG, "Retrying internet verification in 5 seconds...");
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                        break;
                    }
                }
                
                if (verification_done) {
                    ESP_LOGI(TAG, "Internet verification complete, proceeding to certificate check...");
                    s_app_state = APP_STATE_CHECK_CERTIFICATES;
                } else {
                    ESP_LOGW(TAG, "Still waiting for internet verification...");
                }
            }
            break;

        case APP_STATE_CHECK_CERTIFICATES:
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "State: CHECK_CERTIFICATES");
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "Checking if certificates exist in NVS...");
            if (certificate_manager_has_certificates()) {
                ESP_LOGI(TAG, "✓ Certificates found in NVS");
                ESP_LOGI(TAG, "Proceeding to MQTT connection...");
                s_app_state = APP_STATE_MQTT_CONNECTING;
            } else {
                ESP_LOGI(TAG, "Certificates not found in NVS");
                ESP_LOGI(TAG, "Will submit CSR to backend to obtain certificates...");
                s_app_state = APP_STATE_SUBMIT_CSR;
            }
            break;

        case APP_STATE_SUBMIT_CSR:
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "State: SUBMIT_CSR");
            ESP_LOGI(TAG, "========================================");
            {
                static bool csr_submission_attempted = false;
                static int csr_retry_count = 0;
                const int MAX_CSR_RETRIES = 3;
                
                if (!csr_submission_attempted) {
                    char device_id[64] = {0};
                    // JWT provisioning tokens can exceed 256 bytes.
                    char token[1024] = {0};

                    ESP_LOGI(TAG, "Reading provisioning credentials from NVS...");
                    esp_err_t ret = get_provisioning_credentials(device_id, sizeof(device_id),
                                                                 token, sizeof(token));
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "FAILED to get provisioning credentials from NVS!");
                        ESP_LOGE(TAG, "Error: %s", esp_err_to_name(ret));
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "This means device_id or prov_token was not saved during provisioning.");
                        ESP_LOGE(TAG, "Check if /provision endpoint saved credentials correctly.");
                        s_app_state = APP_STATE_ERROR;
                        break;
                    }

                    ESP_LOGI(TAG, "✓ Credentials retrieved from NVS:");
                    ESP_LOGI(TAG, "  Device ID: %s", device_id);
                    ESP_LOGI(TAG, "  Provisioning Token: %.*s... (length: %d)", 
                             token[0] ? 20 : 0, token, strlen(token));
                    ESP_LOGI(TAG, "========================================");
                    ESP_LOGI(TAG, "Submitting CSR to backend server...");
                    ESP_LOGI(TAG, "========================================");

                    ret = certificate_manager_submit_csr(device_id, token);
                    csr_submission_attempted = true;
                    
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "========================================");
                        ESP_LOGI(TAG, "✓ CSR submitted successfully!");
                        ESP_LOGI(TAG, "✓ Certificates saved to NVS");
                        ESP_LOGI(TAG, "========================================");
                        ESP_LOGI(TAG, "✅ STEP: Certificates stored; proceeding to MQTT mTLS connect");
                        csr_retry_count = 0;
                        s_app_state = APP_STATE_MQTT_CONNECTING;
                    } else {
                        csr_retry_count++;
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "✗ CSR submission failed!");
                        ESP_LOGE(TAG, "Error: %s", esp_err_to_name(ret));
                        ESP_LOGE(TAG, "Retry count: %d/%d", csr_retry_count, MAX_CSR_RETRIES);
                        ESP_LOGE(TAG, "========================================");
                        
                        if (csr_retry_count >= MAX_CSR_RETRIES) {
                            ESP_LOGE(TAG, "Maximum retries reached. Moving to error state.");
                            s_app_state = APP_STATE_ERROR;
                        } else {
                            ESP_LOGI(TAG, "Retrying CSR submission in 5 seconds...");
                            csr_submission_attempted = false; // Allow retry
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                    }
                } else {
                    // Already attempted, wait before retry
                    vTaskDelay(pdMS_TO_TICKS(1000));
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
        
        // Heartbeat log every 30 seconds to show state machine is alive
        static int heartbeat_counter = 0;
        heartbeat_counter++;
        if (heartbeat_counter >= 300) { // 300 * 100ms = 30 seconds
            ESP_LOGI(TAG, "[HEARTBEAT] State machine running, current state: %d", s_app_state);
            heartbeat_counter = 0;
        }
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

    // IMPORTANT: Do not clear provisioning data on every boot.
    // Credentials/tokens saved by captive portal must persist so the device can
    // directly proceed to backend CSR + mTLS connection.
    ESP_LOGI(TAG, "Provisioning data persistence enabled (no auto-clear on boot)");

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
