/* WiFi Provisioning Module Implementation
 *
 * Handles WiFi Access Point mode for device provisioning.
 * Provides HTTP endpoints for WiFi scan and credential submission.
 */

#include <string.h>
#include <stdlib.h>
#include "wifi_provisioning.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_prov";

// WiFi scan cache configuration
#define WIFI_SCAN_MAX_APS        20     // Maximum APs to cache

// NVS keys
#define NVS_NAMESPACE "device_config"
#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_PROV_TOKEN "prov_token"
#define NVS_KEY_BEARER_TOKEN "bearer_token"
#define NVS_KEY_PROVISIONED "provisioned"

// Configuration from Kconfig
#define AP_SSID_PREFIX CONFIG_AP_SSID_PREFIX
#define AP_PASSWORD CONFIG_AP_PASSWORD

// Global variables
static httpd_handle_t s_httpd = NULL;
static bool s_provisioning_active = false;
static bool s_wifi_connected = false;
static char s_sta_ip[16] = {0};

// WiFi scan cache (for instant /local-wifi responses)
static wifi_ap_record_t s_cached_networks[WIFI_SCAN_MAX_APS];
static uint16_t s_cached_network_count = 0;
static SemaphoreHandle_t s_cache_mutex = NULL;
static bool s_initial_scan_done = false;

// Forward declarations
static esp_err_t scan_handler(httpd_req_t *req);
static esp_err_t provision_handler(httpd_req_t *req);
static esp_err_t status_handler(httpd_req_t *req);
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
static esp_err_t perform_wifi_scan_and_cache(void);

/**
 * @brief Perform WiFi scan and update cache
 * 
 * This is called ONCE during provisioning startup (before any client connects)
 * and optionally on-demand via /local-wifi?refresh=true
 * 
 * NO background scanning = stable AP connection for connected clients
 */
static esp_err_t perform_wifi_scan_and_cache(void)
{
    ESP_LOGI(TAG, "Performing WiFi scan...");
    
    // Create mutex if not exists
    if (s_cache_mutex == NULL) {
        s_cache_mutex = xSemaphoreCreateMutex();
        if (s_cache_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create cache mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            }
        }
    };
    
    // Perform WiFi scan (blocking)
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Get scan results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WIFI_SCAN_MAX_APS) {
        ap_count = WIFI_SCAN_MAX_APS;
    }
    
    // Update cache with mutex protection
    if (xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        s_cached_network_count = ap_count;
        if (ap_count > 0) {
            esp_wifi_scan_get_ap_records(&s_cached_network_count, s_cached_networks);
        }
        s_initial_scan_done = true;
        xSemaphoreGive(s_cache_mutex);
        
        ESP_LOGI(TAG, "WiFi scan completed: %d networks cached", s_cached_network_count);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex for cache update");
        return ESP_ERR_TIMEOUT;
    }
    
    return ESP_OK;
}

/**
 * @brief Save WiFi credentials to NVS
 */
static esp_err_t save_wifi_credentials(const char *ssid, const char *password,
                                       const char *device_id, const char *prov_token,
                                       const char *bearer_token)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_WIFI_SSID, ssid);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_str(nvs_handle, NVS_KEY_WIFI_PASS, password);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id);
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_str(nvs_handle, NVS_KEY_PROV_TOKEN, prov_token);
    if (err != ESP_OK) goto cleanup;

    // Save Bearer token if provided
    if (bearer_token != NULL && strlen(bearer_token) > 0) {
        err = nvs_set_str(nvs_handle, NVS_KEY_BEARER_TOKEN, bearer_token);
        if (err != ESP_OK) goto cleanup;
        ESP_LOGI(TAG, "Bearer token saved to NVS");
    } else {
        ESP_LOGW(TAG, "No Bearer token provided");
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_PROVISIONED, 1);
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(nvs_handle);

cleanup:
    nvs_close(nvs_handle);
    return err;
}

/**
 * @brief HTTP GET handler for /local-wifi endpoint
 * 
 * Returns cached WiFi scan results instantly (low latency UX).
 * Cache is populated ONCE at startup. No background scanning = stable AP.
 * 
 * Optional: /local-wifi?refresh=true to force a new scan (will briefly disrupt connection)
 */
static esp_err_t scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "WiFi scan requested");

    // Check for refresh parameter
    char query[32] = {0};
    bool force_refresh = false;
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16] = {0};
        if (httpd_query_key_value(query, "refresh", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "true") == 0 || strcmp(param, "1") == 0) {
                force_refresh = true;
                ESP_LOGW(TAG, "Force refresh requested - this will briefly disrupt WiFi");
            }
        }
    }

    // If cache is empty or force refresh requested, do a scan
    if (!s_initial_scan_done || force_refresh) {
        ESP_LOGI(TAG, "Performing WiFi scan (cache %s)...", 
                 force_refresh ? "refresh requested" : "empty");
        
        esp_err_t ret = perform_wifi_scan_and_cache();
        if (ret != ESP_OK && !s_initial_scan_done) {
            // Only fail if we have no cached data at all
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"scan_failed\",\"message\":\"No cached data available\"}");
            return ESP_FAIL;
        }
    }

    // Take mutex to safely read cache
    if (s_cache_mutex == NULL || xSemaphoreTake(s_cache_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire cache mutex");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"cache_busy\"}");
        return ESP_FAIL;
    }

    // Build JSON response from cached data
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < s_cached_network_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char*)s_cached_networks[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", s_cached_networks[i].rssi);
        cJSON_AddNumberToObject(network, "channel", s_cached_networks[i].primary);
        cJSON_AddBoolToObject(network, "secure", s_cached_networks[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
    }

    uint16_t count = s_cached_network_count;
    
    // Release mutex after reading
    xSemaphoreGive(s_cache_mutex);

    cJSON_AddItemToObject(root, "networks", networks);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddBoolToObject(root, "cached", !force_refresh);  // false if just refreshed

    char *json_string = cJSON_Print(root);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON string");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"json_error\"}");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    ESP_LOGI(TAG, "Returned %d networks (instant response)", count);

    free(json_string);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief HTTP POST handler for /provision endpoint
 */
static esp_err_t provision_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Provisioning request received");

    // Extract Authorization header (Bearer token)
    char auth_header[512] = {0};
    const char *bearer_token = NULL;
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    
    if (auth_len > 1) {
        if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, auth_len) == ESP_OK) {
            ESP_LOGI(TAG, "Authorization header received: %s", auth_header);
            
            // Extract Bearer token (skip "Bearer " prefix if present, case-insensitive)
            if (strncasecmp(auth_header, "Bearer ", 7) == 0) {
                bearer_token = auth_header + 7;  // Skip "Bearer " prefix
            } else {
                bearer_token = auth_header;  // Use as-is if no "Bearer " prefix
            }
            ESP_LOGI(TAG, "Extracted Bearer token: %s", bearer_token);
        } else {
            ESP_LOGW(TAG, "Failed to read Authorization header");
        }
    } else {
        ESP_LOGW(TAG, "No Authorization header provided");
    }

    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_request\"}");
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_json = cJSON_GetObjectItem(root, "password");
    cJSON *device_id_json = cJSON_GetObjectItem(root, "device_id");
    cJSON *token_json = cJSON_GetObjectItem(root, "provisioning_token");

    if (!cJSON_IsString(ssid_json) || !cJSON_IsString(password_json) ||
        !cJSON_IsString(device_id_json) || !cJSON_IsString(token_json)) {
        ESP_LOGE(TAG, "Missing required fields");
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"missing_fields\"}");
        return ESP_FAIL;
    }

    const char *ssid = ssid_json->valuestring;
    const char *password = password_json->valuestring;
    const char *device_id = device_id_json->valuestring;
    const char *prov_token = token_json->valuestring;

    ESP_LOGI(TAG, "Received credentials - SSID: %s, Device ID: %s", ssid, device_id);

    // Save credentials to NVS (including Bearer token from Authorization header)
    esp_err_t err = save_wifi_credentials(ssid, password, device_id, prov_token, bearer_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"save_failed\"}");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Credentials saved\"}");

    // Stop HTTP server and switch to STA mode
    vTaskDelay(pdMS_TO_TICKS(500)); // Give time for response to be sent
    wifi_provisioning_stop();

    // Configure and connect to WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    return ESP_OK;
}

/**
 * @brief HTTP GET handler for /status endpoint
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (s_wifi_connected) {
        cJSON_AddStringToObject(root, "status", "connected");
        cJSON_AddStringToObject(root, "ip", s_sta_ip);
    } else if (s_provisioning_active) {
        cJSON_AddStringToObject(root, "status", "provisioning");
        cJSON_AddStringToObject(root, "ip", "192.168.4.1");
    } else {
        cJSON_AddStringToObject(root, "status", "disconnected");
    }

    char *json_string = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_string);

    free(json_string);
    cJSON_Delete(root);

    return ESP_OK;
}

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station "MACSTR" connected, AID=%d",
                         MAC2STR(event->mac), event->aid);
            }
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station "MACSTR" disconnected, AID=%d, reason=%d",
                         MAC2STR(event->mac), event->aid, event->reason);
            }
            break;
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "WiFi STA connected");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            {
                wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
                ESP_LOGI(TAG, "WiFi STA disconnected, reason: %d", event->reason);
                s_wifi_connected = false;
                memset(s_sta_ip, 0, sizeof(s_sta_ip));
                
                // Check for authentication failures
                // Common auth failure reason codes:
                // 15 = WIFI_REASON_AUTH_FAIL
                // 201 = WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                // 202 = WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT
                // 203 = WIFI_REASON_IE_IN_4WAY_DIFFERS
                // 204 = WIFI_REASON_GROUP_CIPHER_INVALID
                // 205 = WIFI_REASON_PAIRWISE_CIPHER_INVALID
                // 206 = WIFI_REASON_AKMP_INVALID
                // 207 = WIFI_REASON_UNSUPP_RSN_IE_VERSION
                // 208 = WIFI_REASON_INVALID_RSN_IE_CAP
                // 209 = WIFI_REASON_802_1X_AUTH_FAILED
                if (event->reason == 15 || (event->reason >= 201 && event->reason <= 209)) {
                    ESP_LOGE(TAG, "========================================");
                    ESP_LOGE(TAG, "✗ WiFi Authentication Failed!");
                    ESP_LOGE(TAG, "✗ Reason Code: %d", event->reason);
                    ESP_LOGE(TAG, "✗ Incorrect WiFi credentials provided");
                    ESP_LOGE(TAG, "========================================");
                    ESP_LOGI(TAG, "Clearing invalid credentials...");
                    ESP_LOGI(TAG, "Returning to AP mode...");
                    ESP_LOGI(TAG, "Please send new credentials via HTTP POST /provision");
                    
                    // Clear credentials and return to AP mode
                    wifi_provisioning_clear_and_restart();
                } else {
                    ESP_LOGW(TAG, "WiFi disconnected (reason: %d) - may retry", event->reason);
                }
            }
            break;
        default:
            break;
        }
    }
}

/**
 * @brief IP event handler
 */
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_AP_STAIPASSIGNED:
            {
                ip_event_ap_staipassigned_t* event = (ip_event_ap_staipassigned_t*) event_data;
                ESP_LOGI(TAG, "AP assigned IP " IPSTR " to station", IP2STR(&event->ip));
            }
            break;
        case IP_EVENT_STA_GOT_IP:
            {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
                ESP_LOGI(TAG, "Got IP: %s", s_sta_ip);
                s_wifi_connected = true;
            }
            break;
        default:
            break;
        }
    }
}

/**
 * @brief Start HTTP server
 */
static httpd_handle_t start_http_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    
    // Increase timeouts for long-running operations like WiFi scan (15-20 seconds)
    config.recv_wait_timeout = 30;  // 30 seconds receive timeout
    config.send_wait_timeout = 30;  // 30 seconds send timeout

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t scan_uri = {
            .uri = "/local-wifi",
            .method = HTTP_GET,
            .handler = scan_handler,
        };
        httpd_register_uri_handler(server, &scan_uri);

        httpd_uri_t provision_uri = {
            .uri = "/provision",
            .method = HTTP_POST,
            .handler = provision_handler,
        };
        httpd_register_uri_handler(server, &provision_uri);

        httpd_uri_t status_uri = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_handler,
        };
        httpd_register_uri_handler(server, &status_uri);

        ESP_LOGI(TAG, "HTTP server started");
        return server;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

/**
 * @brief Initialize WiFi in AP mode
 */
static esp_err_t wifi_init_ap(void)
{
    // Create default WiFi AP netif
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &ip_event_handler,
                                                        NULL,
                                                        NULL));

    // Configure AP
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID_PREFIX,
            .ssid_len = strlen(AP_SSID_PREFIX),
            .password = AP_PASSWORD,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    if (strlen(AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Start in APSTA mode so we can scan without stopping WiFi
    // This prevents connection resets when /local-wifi endpoint is called
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Password=%s", AP_SSID_PREFIX, AP_PASSWORD);
    return ESP_OK;
}

// Public API implementation

esp_err_t wifi_provisioning_start(void)
{
    if (s_provisioning_active) {
        ESP_LOGW(TAG, "Provisioning already active");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting WiFi provisioning");

    // Initialize network interface if not already done
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create event loop if not already done
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize WiFi AP
    ret = wifi_init_ap();
    if (ret != ESP_OK) {
        return ret;
    }

    // Perform initial WiFi scan BEFORE starting HTTP server
    // This ensures cache is populated before any client connects
    ESP_LOGI(TAG, "Performing initial WiFi scan (before clients connect)...");
    esp_err_t scan_ret = perform_wifi_scan_and_cache();
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial scan failed, will retry on first /local-wifi request");
    }

    // Start HTTP server (clients can now connect with stable AP)
    s_httpd = start_http_server();
    if (s_httpd == NULL) {
        esp_wifi_stop();
        return ESP_FAIL;
    }

    s_provisioning_active = true;
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WiFi provisioning started successfully");
    ESP_LOGI(TAG, "AP is stable - no background scanning");
    ESP_LOGI(TAG, "/local-wifi returns cached results instantly");
    ESP_LOGI(TAG, "Use /local-wifi?refresh=true to rescan");
    ESP_LOGI(TAG, "========================================");
    return ESP_OK;
}

esp_err_t wifi_provisioning_stop(void)
{
    if (!s_provisioning_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping WiFi provisioning");

    // Stop HTTP server
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    // Reset scan cache state
    s_initial_scan_done = false;
    s_cached_network_count = 0;

    s_provisioning_active = false;
    return ESP_OK;
}

bool wifi_provisioning_is_provisioned(void)
{
    nvs_handle_t nvs_handle;
    uint8_t provisioned = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        return false;
    }

    nvs_get_u8(nvs_handle, NVS_KEY_PROVISIONED, &provisioned);
    nvs_close(nvs_handle);

    return provisioned == 1;
}

bool wifi_provisioning_get_status(char *ip_addr, size_t ip_len)
{
    if (s_wifi_connected && ip_addr && ip_len > 0) {
        strncpy(ip_addr, s_sta_ip, ip_len - 1);
        ip_addr[ip_len - 1] = '\0';
        return true;
    }
    return false;
}

esp_err_t wifi_provisioning_get_bearer_token(char *token, size_t token_len)
{
    if (token == NULL || token_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    size_t required_size = token_len;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_BEARER_TOKEN, token, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Bearer token retrieved from NVS (%d bytes)", required_size);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Bearer token not found in NVS");
    } else {
        ESP_LOGE(TAG, "Failed to get Bearer token: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t wifi_provisioning_clear_and_restart(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Clearing provisioning credentials");
    ESP_LOGI(TAG, "Returning to AP mode for new credentials");
    ESP_LOGI(TAG, "========================================");

    // Stop HTTP server if running
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    
    // Reset scan cache state
    s_initial_scan_done = false;
    s_cached_network_count = 0;
    
    // Reset provisioning active flag
    s_provisioning_active = false;

    // Clear all provisioning data from NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erasing provisioning data from NVS...");
        
        nvs_erase_key(nvs_handle, NVS_KEY_PROVISIONED);
        nvs_erase_key(nvs_handle, NVS_KEY_WIFI_SSID);
        nvs_erase_key(nvs_handle, NVS_KEY_WIFI_PASS);
        nvs_erase_key(nvs_handle, NVS_KEY_DEVICE_ID);
        nvs_erase_key(nvs_handle, NVS_KEY_PROV_TOKEN);
        nvs_erase_key(nvs_handle, NVS_KEY_BEARER_TOKEN);
        
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "✓ Provisioning data cleared");
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for clearing: %s", esp_err_to_name(err));
    }

    // Stop WiFi STA mode
    esp_wifi_stop();
    s_wifi_connected = false;
    memset(s_sta_ip, 0, sizeof(s_sta_ip));
    
    // Restart provisioning AP
    vTaskDelay(pdMS_TO_TICKS(1000)); // Give time for WiFi to stop
    err = wifi_provisioning_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart provisioning AP: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "✓ Provisioning AP restarted");
    ESP_LOGI(TAG, "✓ Waiting for new credentials via HTTP POST /provision");
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

