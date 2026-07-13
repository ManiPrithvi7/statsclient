/* WiFi Provisioning with mTLS MQTT - Main Application
 *
 * This application implements a complete provisioning flow:
 * 1. Boot → Check if device is provisioned
 * 2. If not provisioned → Start AP mode with HTTP server for provisioning
 * 3. After provisioning → Connect to WiFi
 * 4. Submit CSR to backend and receive certificates
 * 5. Connect to MQTT broker using mTLS
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_provisioning.h"
#include "certificate_manager.h"
#include "internet_verification.h"
#include "mqtt_handler.h"
#include "ota_handler.h"
#include "screen_handler.h"
#include "device_keys.h"
#include "prov_token_validate.h"

static const char *TAG = "main";

/** State-machine flags (reset together when forcing captive-portal reprovision). */
static bool s_wifi_connection_attempted;
static bool s_wifi_sta_inited;
static bool s_verification_done;
static int s_verification_retries;
static bool s_prov_token_expiry_checked;
static bool s_csr_submission_attempted;
static int s_csr_retry_count;
static int s_mqtt_connect_retries;
static bool s_mqtt_connected_msg_shown;
static bool s_ota_started;
static int s_wifi_connect_wait_seconds;
static bool s_wifi_auth_failed;
static const int WIFI_CONNECT_TIMEOUT_SECONDS = 50;

/** ponytail: 44 = 11 dBm; lowers USB brownout risk during WiFi assoc */
#define WIFI_STA_TX_POWER_QUARTER_DBM 44

static void on_mqtt_connected(void)
{
    ota_handler_on_mqtt_connected();
    screen_handler_start();
}

static void on_mqtt_disconnected(void)
{
    screen_handler_stop();
}

static void main_reset_cycle_for_reprovision(void)
{
    s_wifi_connection_attempted = false;
    s_wifi_sta_inited = false;
    s_verification_done = false;
    s_verification_retries = 0;
    s_prov_token_expiry_checked = false;
    s_csr_submission_attempted = false;
    s_csr_retry_count = 0;
    s_mqtt_connect_retries = 0;
    s_mqtt_connected_msg_shown = false;
    s_ota_started = false;
    s_wifi_connect_wait_seconds = 0;
    s_wifi_auth_failed = false;
}

static bool wifi_sta_is_auth_failure(int reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           (reason >= WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT &&
            reason <= WIFI_REASON_802_1X_AUTH_FAILED);
}

static const char *wifi_authmode_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    default: return "?";
    }
}

static void wifi_sta_log_target_ap(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)ssid,
        .show_hidden = true,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "Pre-connect scan failed");
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "Scan: '%s' not found (enable 2.4 GHz hotspot)", ssid);
        return;
    }

    wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (aps == NULL) {
        return;
    }
    esp_wifi_scan_get_ap_records(&ap_count, aps);
    for (uint16_t i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "Scan: '%s' rssi=%d ch=%u auth=%d (%s)",
                 (char *)aps[i].ssid, aps[i].rssi, aps[i].primary,
                 aps[i].authmode, wifi_authmode_name(aps[i].authmode));
    }
    free(aps);
}

static void wifi_sta_fill_config(wifi_config_t *cfg, const char *ssid, const char *password)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy((char *)cfg->sta.ssid, ssid, sizeof(cfg->sta.ssid) - 1);
    strncpy((char *)cfg->sta.password, password, sizeof(cfg->sta.password) - 1);
    cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

static void ota_verify_task(void *arg)
{
    (void)arg;
    ota_handler_run_pending_verify();
    vTaskDelete(NULL);
}

static void trim_spaces_in_place(char *s)
{
    if (s == NULL || s[0] == '\0') {
        return;
    }

    char *start = s;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void force_reprovision_due_to_token(const char *reason)
{
    ESP_LOGW(TAG, "Provisioning token check failed: %s", reason);
    ESP_LOGI(TAG, "✅ STEP: Clearing NVS and restarting captive portal (AP)");
    (void)certificate_manager_erase_stored_certificates();
    (void)wifi_provisioning_erase_stored_credentials();
    main_reset_cycle_for_reprovision();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
}

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
        s_wifi_connect_wait_seconds = 0;
        s_wifi_auth_failed = false;
        s_app_state = APP_STATE_WIFI_CONNECTED;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connect_wait_seconds = 0;
        s_wifi_auth_failed = false;
        s_app_state = APP_STATE_WIFI_CONNECTED;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi STA disconnected, reason=%d", event->reason);
        if (wifi_provisioning_dev_wifi_enabled()) {
            if (wifi_sta_is_auth_failure(event->reason)) {
                s_wifi_auth_failed = true;
                ESP_LOGE(TAG, "WiFi auth failed — verify CONFIG_DEV_WIFI_PASSWORD and hotspot WPA2/2.4GHz");
            }
            s_wifi_connect_wait_seconds = 0;
            esp_wifi_connect();
        }
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
            {
                if (wifi_provisioning_dev_wifi_enabled()) {
                    (void)wifi_provisioning_dev_wifi_sync_nvs();
                    ESP_LOGI(TAG, "Dev WiFi: sdkconfig STA-only (no AP)");
                    s_app_state = APP_STATE_WIFI_CONNECTING;
                } else if (wifi_provisioning_is_provisioned()) {
                    ESP_LOGI(TAG, "Device is provisioned, connecting to WiFi...");
                    s_app_state = APP_STATE_WIFI_CONNECTING;
                } else {
                    ESP_LOGI(TAG, "Device not provisioned, starting AP mode...");
                    s_app_state = APP_STATE_AP_MODE;
                }
            }
            break;

        case APP_STATE_AP_MODE:
            ESP_LOGI(TAG, "State: AP_MODE");
            {
                // If provisioning handler saved credentials, it sets `provisioned=1`.
                // This should be the primary signal to leave AP_MODE.
                if (wifi_provisioning_is_provisioned()) {
                    ESP_LOGI(TAG, "✅ STEP: provisioning flag found in NVS; skipping AP wait");
                    (void)wifi_provisioning_stop(); // stop HTTP/DNS side of provisioning
                    s_app_state = APP_STATE_WIFI_CONNECTING;
                    break;
                }

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
            {
                if (!s_wifi_connection_attempted) {
                    ESP_LOGI(TAG, "State: WIFI_CONNECTING");
                    // When we skip AP mode (credential reuse), WiFi was never initialized.
                    // Ensure STA netif exists and esp_wifi_init() has been called before connecting.
                    if (!s_wifi_sta_inited) {
                        esp_netif_create_default_wifi_sta();
                        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                        esp_err_t err = esp_wifi_init(&cfg);
                        if (err != ESP_OK) {
                            ESP_LOGW(TAG, "esp_wifi_init: %s (continuing in case already inited)", esp_err_to_name(err));
                        }
                        s_wifi_sta_inited = true;
                        ESP_LOGI(TAG, "✅ STEP: WiFi driver initialized for STA");
                    }

                    // Load credentials: dev mode uses sdkconfig directly (not stale NVS).
                    char ssid[33] = {0};
                    char password[65] = {0};
                    bool have_creds = false;

                    if (wifi_provisioning_dev_wifi_enabled()) {
                        strncpy(ssid, CONFIG_DEV_WIFI_SSID, sizeof(ssid) - 1);
                        strncpy(password, CONFIG_DEV_WIFI_PASSWORD, sizeof(password) - 1);
                        have_creds = ssid[0] != '\0';
                    } else {
                        nvs_handle_t nvs_handle;
                        if (nvs_open("device_config", NVS_READONLY, &nvs_handle) == ESP_OK) {
                            size_t required_size = sizeof(ssid);
                            if (nvs_get_str(nvs_handle, "wifi_ssid", ssid, &required_size) == ESP_OK) {
                                required_size = sizeof(password);
                                nvs_get_str(nvs_handle, "wifi_pass", password, &required_size);
                                have_creds = true;
                            }
                            nvs_close(nvs_handle);
                        }
                    }

                    if (have_creds) {
                        trim_spaces_in_place(ssid);
                        trim_spaces_in_place(password);

                        wifi_config_t wifi_config;
                        wifi_sta_fill_config(&wifi_config, ssid, password);

                        ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
                        ESP_LOGI(TAG, "✅ STEP: WiFi credentials loaded; attempting STA connection");

                        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            break;
                        }
                        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            break;
                        }
                        err = esp_wifi_start();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            break;
                        }
                        (void)esp_wifi_set_max_tx_power(WIFI_STA_TX_POWER_QUARTER_DBM);
                        wifi_sta_log_target_ap(ssid);
                        err = esp_wifi_connect();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            break;
                        }

                        s_wifi_connection_attempted = true;
                    } else {
                        ESP_LOGE(TAG, "No WiFi credentials available");
                        if (wifi_provisioning_dev_wifi_enabled()) {
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        } else {
                            s_app_state = APP_STATE_AP_MODE;
                        }
                    }
                } else {
                    s_wifi_connect_wait_seconds++;
                    if ((s_wifi_connect_wait_seconds % 5) == 0) {
                        ESP_LOGI(TAG, "State: WIFI_CONNECTING (%d s elapsed)", s_wifi_connect_wait_seconds);
                    }
                    if ((s_wifi_connect_wait_seconds % 10) == 0) {
                        esp_err_t retry = esp_wifi_connect();
                        if (retry != ESP_OK) {
                            ESP_LOGW(TAG, "WiFi reconnect nudge failed: %s", esp_err_to_name(retry));
                        }
                    }
                    if (s_wifi_connect_wait_seconds >= WIFI_CONNECT_TIMEOUT_SECONDS) {
                        if (wifi_provisioning_dev_wifi_enabled()) {
                            ESP_LOGW(TAG, "WiFi connect timeout (%d s) — dev mode, retrying STA",
                                     WIFI_CONNECT_TIMEOUT_SECONDS);
                            s_wifi_connection_attempted = false;
                            s_wifi_connect_wait_seconds = 0;
                            esp_wifi_disconnect();
                            esp_wifi_stop();
                            vTaskDelay(pdMS_TO_TICKS(1000));
                        } else {
                            ESP_LOGW(TAG, "WiFi connect timeout (%d s). Restarting AP provisioning for new credentials.",
                                     WIFI_CONNECT_TIMEOUT_SECONDS);
                            main_reset_cycle_for_reprovision();
                            esp_err_t prov_ret = wifi_provisioning_clear_and_restart();
                            if (prov_ret != ESP_OK) {
                                ESP_LOGE(TAG, "Failed to restart provisioning AP: %s", esp_err_to_name(prov_ret));
                                s_app_state = APP_STATE_ERROR;
                            } else {
                                s_app_state = APP_STATE_AP_MODE;
                            }
                        }
                        break;
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
                const int MAX_VERIFICATION_RETRIES = 2; // Try 2 times before giving up

                // Reset verification state if we're not provisioned (means we returned to AP mode)
                if (!wifi_provisioning_is_provisioned() && !wifi_provisioning_dev_wifi_enabled()) {
                    ESP_LOGW(TAG, "Device not provisioned, resetting to AP mode");
                    main_reset_cycle_for_reprovision();
                    s_app_state = APP_STATE_AP_MODE;
                    break;
                }

                /* Short-lived provisioning JWT: after SNTP, drop stale tokens and reopen portal. */
                if (!s_prov_token_expiry_checked) {
                    s_prov_token_expiry_checked = true;

#if CONFIG_USE_EMBEDDED_MTLS_CERTS
                    ESP_LOGI(TAG, "Embedded mTLS certs enabled — skipping JWT expiry check");
#else
                    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                    esp_err_t sn_init = esp_netif_sntp_init(&sntp_cfg);
                    esp_err_t sn = ESP_FAIL;
                    if (sn_init != ESP_OK) {
                        ESP_LOGW(TAG, "SNTP init failed: %s — skipping JWT expiry check",
                                 esp_err_to_name(sn_init));
                    } else {
                        sn = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
                        esp_netif_sntp_deinit();
                        if (sn != ESP_OK) {
                            ESP_LOGW(TAG, "SNTP sync via pool.ntp.org failed (%s), retrying with time.google.com",
                                     esp_err_to_name(sn));
                            esp_sntp_config_t sntp_cfg_fallback = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
                            if (esp_netif_sntp_init(&sntp_cfg_fallback) == ESP_OK) {
                                sn = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000));
                                esp_netif_sntp_deinit();
                            }
                        }
                    }

                    time_t now = time(NULL);
                    const time_t k_min_plausible_utc = 1577836800; /* 2020-01-01 */
                    bool time_trusted = (sn == ESP_OK && now > k_min_plausible_utc);

                    if (time_trusted) {
                        char token[1024] = {0};
                        char device_id[64] = {0};
                        esp_err_t cr = get_provisioning_credentials(device_id, sizeof(device_id),
                                                                  token, sizeof(token));
                        if (cr == ESP_OK) {
                            prov_token_status_t st = prov_token_check_expiry(token, now);
                            if (st == PROV_TOKEN_STATUS_EXPIRED) {
                                force_reprovision_due_to_token("token expired; need fresh token from web app");
                                s_app_state = APP_STATE_AP_MODE;
                                break;
                            }
                            if (st == PROV_TOKEN_STATUS_MALFORMED) {
                                force_reprovision_due_to_token("token malformed (missing/invalid exp/iat)");
                                s_app_state = APP_STATE_AP_MODE;
                                break;
                            }
                        } else {
                            force_reprovision_due_to_token("unable to read provisioning token from NVS");
                            s_app_state = APP_STATE_AP_MODE;
                            break;
                        }
                    } else {
                        force_reprovision_due_to_token("time not trusted after SNTP; cannot validate token age");
                        s_app_state = APP_STATE_AP_MODE;
                        break;
                    }
#endif
                }

                if (!s_verification_done) {
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
                        s_verification_done = true;
                        s_verification_retries = 0; // Reset retry counter
                    } else {
                        s_verification_retries++;
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "✗ Internet verification failed!");
                        ESP_LOGE(TAG, "✗ Error: %s", esp_err_to_name(ret));
                        ESP_LOGE(TAG, "✗ Retry attempt: %d/%d", s_verification_retries, MAX_VERIFICATION_RETRIES);
                        ESP_LOGE(TAG, "========================================");
                        
                        if (s_verification_retries >= MAX_VERIFICATION_RETRIES) {
                            ESP_LOGE(TAG, "Maximum retries reached. Credentials may be incorrect.");
                            ESP_LOGE(TAG, "WiFi may be connected but has no internet access.");
                            ESP_LOGI(TAG, "NOTE: Proceeding to certificate check anyway...");
                            ESP_LOGI(TAG, "CSR submission will be attempted even without internet verification.");
                            
                            // Instead of clearing credentials, proceed to certificate check
                            // This allows CSR submission to be attempted
                            s_verification_done = true; // Mark as done to proceed
                            s_verification_retries = 0;
                        } else {
                            ESP_LOGW(TAG, "Retrying internet verification in 5 seconds...");
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                        break;
                    }
                }
                
                if (s_verification_done) {
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
            ESP_LOGI(TAG, "Checking for mTLS certificates (embedded mtls_client/certs or NVS)...");
            if (certificate_manager_has_certificates()) {
                ESP_LOGI(TAG, "✓ mTLS certificates available");
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
                const int MAX_CSR_RETRIES = 3;
                
                if (!s_csr_submission_attempted) {
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
                    s_csr_submission_attempted = true;
                    
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "========================================");
                        ESP_LOGI(TAG, "✓ CSR submitted successfully!");
                        ESP_LOGI(TAG, "✓ Certificates saved to NVS");
                        ESP_LOGI(TAG, "========================================");
                        ESP_LOGI(TAG, "✅ STEP: Certificates stored; proceeding to MQTT mTLS connect");
                        s_csr_retry_count = 0;
                        s_app_state = APP_STATE_MQTT_CONNECTING;
                    } else {
                        s_csr_retry_count++;
                        ESP_LOGE(TAG, "========================================");
                        ESP_LOGE(TAG, "✗ CSR submission failed!");
                        ESP_LOGE(TAG, "Error: %s", esp_err_to_name(ret));
                        ESP_LOGE(TAG, "Retry count: %d/%d", s_csr_retry_count, MAX_CSR_RETRIES);
                        ESP_LOGE(TAG, "========================================");
                        
                        if (s_csr_retry_count >= MAX_CSR_RETRIES) {
                            ESP_LOGE(TAG, "Maximum retries reached. Moving to error state.");
                            s_app_state = APP_STATE_ERROR;
                        } else {
                            ESP_LOGI(TAG, "Retrying CSR submission in 5 seconds...");
                            s_csr_submission_attempted = false; // Allow retry
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
                const int MAX_MQTT_RETRIES = 3;

                if (!s_ota_started) {
                    char device_id[64] = {0};
                    char token[16] = {0};
                    if (get_provisioning_credentials(device_id, sizeof(device_id),
                                                     token, sizeof(token)) != ESP_OK) {
#if CONFIG_USE_EMBEDDED_MTLS_CERTS
                        strncpy(device_id, CONFIG_MTLS_CLIENT_DEVICE_ID, sizeof(device_id) - 1);
                        device_id[sizeof(device_id) - 1] = '\0';
                        ESP_LOGI(TAG, "Using embedded mTLS device_id for MQTT: %s", device_id);
#else
                        ESP_LOGE(TAG, "No device_id in NVS; cannot start MQTT");
                        s_app_state = APP_STATE_ERROR;
                        break;
#endif
                    }
                    mqtt_handler_set_device_id(device_id);
                    mqtt_handler_set_cmd_callback(ota_handler_on_mqtt_cmd);
                    mqtt_handler_set_ack_callback(ota_handler_on_mqtt_ack);
                    mqtt_handler_set_connected_callback(on_mqtt_connected);
                    mqtt_handler_set_disconnected_callback(on_mqtt_disconnected);
                    mqtt_handler_set_screen_callback(screen_handler_on_mqtt);
                    (void)screen_handler_init();
                    (void)ota_handler_init(device_id);
                    (void)ota_handler_start();
                    s_ota_started = true;
                }

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
                        s_mqtt_connect_retries = 0;
                        s_app_state = APP_STATE_MQTT_CONNECTED;
                    } else {
                        ESP_LOGW(TAG, "MQTT connection timeout");
                        mqtt_handler_stop();
                        s_mqtt_connect_retries++;
                        
                        if (s_mqtt_connect_retries >= MAX_MQTT_RETRIES) {
                            ESP_LOGE(TAG, "MQTT connection failed after %d retries", MAX_MQTT_RETRIES);
                            s_app_state = APP_STATE_ERROR;
                        } else {
                            ESP_LOGI(TAG, "Retrying MQTT connection... (%d/%d)", s_mqtt_connect_retries, MAX_MQTT_RETRIES);
                            vTaskDelay(pdMS_TO_TICKS(5000));
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to start MQTT handler: %s", esp_err_to_name(ret));
                    s_mqtt_connect_retries++;
                    
                    if (s_mqtt_connect_retries >= MAX_MQTT_RETRIES) {
                        s_app_state = APP_STATE_ERROR;
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                }
            }
            break;

        case APP_STATE_MQTT_CONNECTED:
            {
                if (!s_mqtt_connected_msg_shown) {
                    ESP_LOGI(TAG, "========================================");
                    ESP_LOGI(TAG, "State: MQTT_CONNECTED");
                    ESP_LOGI(TAG, "========================================");
                    ESP_LOGI(TAG, "✓ Device provisioning complete!");
                    ESP_LOGI(TAG, "✓ mTLS MQTT connection established!");
                    ESP_LOGI(TAG, "✓ Device is fully operational!");
                    ESP_LOGI(TAG, "========================================");
                    s_mqtt_connected_msg_shown = true;

                    if (ota_handler_pending_verify_active()) {
                        xTaskCreate(ota_verify_task, "ota_verify", 4096, NULL, 4, NULL);
                    }
                }

                if (!mqtt_handler_is_connected()) {
                    ESP_LOGW(TAG, "MQTT disconnected — waiting for auto-reconnect");
                    vTaskDelay(pdMS_TO_TICKS(2000));
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

#if CONFIG_ENABLE_DWIN_DISPLAY
    {
        esp_err_t screen_err = screen_handler_init();
        if (screen_err == ESP_OK) {
            ESP_LOGI(TAG, "DWIN UART initialized at boot");
        } else {
            ESP_LOGW(TAG, "DWIN init at boot failed: %s", esp_err_to_name(screen_err));
        }
    }
#endif

    // Start state machine task
    xTaskCreate(app_state_machine_task, "app_state_machine", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "State machine task started");

    ESP_LOGI(TAG, "Application initialization complete");
}
