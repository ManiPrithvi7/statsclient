#include "ota_handler.h"

#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http_mtls_client.h"
#include "mbedtls/sha256.h"
#include "mqtt_handler.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_manifest.h"
#include "ota_signing.h"

static const char *TAG = "ota_handler";

#define OTA_NVS_NAMESPACE "proof_device"
#define OTA_NVS_LAST_DOWNLOAD "ota_dl_ts"
#define OTA_COOLDOWN_SEC 3600
#define OTA_TASK_STACK 8192
#define OTA_TASK_PRIO 5
#define OTA_ROLLBACK_ACK_BIT BIT0
#define OTA_WORK_CHECK BIT0
#define OTA_WORK_UPDATE BIT1

static char s_device_id[64];
static bool s_initialized;
static TaskHandle_t s_ota_task;
static EventGroupHandle_t s_ota_events;
static EventGroupHandle_t s_rollback_ack_events;
static esp_timer_handle_t s_poll_timer;
static ota_manifest_t s_pending_manifest;
static bool s_force_download;
static bool s_pending_verify_mode;
static char s_pending_version[32];
static char s_inflight_version[32];

typedef struct {
    ota_manifest_t manifest;
    bool force;
} ota_work_item_t;

static bool cooldown_active(void)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    int64_t last_ts = 0;
    esp_err_t err = nvs_get_i64(nvs, OTA_NVS_LAST_DOWNLOAD, &last_ts);
    nvs_close(nvs);
    if (err != ESP_OK || last_ts <= 0) {
        return false;
    }

    time_t now = time(NULL);
    if (now <= 0) {
        return false;
    }
    return (now - (time_t)last_ts) < OTA_COOLDOWN_SEC;
}

static esp_err_t record_download_timestamp(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    int64_t now = (int64_t)time(NULL);
    err = nvs_set_i64(nvs, OTA_NVS_LAST_DOWNLOAD, now);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void copy_json_string(char *dest, size_t dest_len, cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dest, item->valuestring, dest_len - 1);
        dest[dest_len - 1] = '\0';
    }
}

static esp_err_t parse_manifest_from_json(cJSON *root, ota_manifest_t *out)
{
    if (root == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    copy_json_string(out->version, sizeof(out->version), root, "version");
    copy_json_string(out->download_url, sizeof(out->download_url), root, "download_url");
    copy_json_string(out->sha256, sizeof(out->sha256), root, "sha256");
    copy_json_string(out->signature, sizeof(out->signature), root, "signature");

    cJSON *size = cJSON_GetObjectItem(root, "size_bytes");
    if (cJSON_IsNumber(size)) {
        out->size_bytes = (uint32_t)size->valuedouble;
    }

    if (out->version[0] == '\0' || out->download_url[0] == '\0' ||
        out->sha256[0] == '\0' || out->signature[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t publish_status_json(cJSON *obj)
{
    char *json = cJSON_PrintUnformatted(obj);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = mqtt_handler_publish_status_json(json);
    cJSON_free(json);
    return err;
}

static esp_err_t ota_handler_check_for_update(ota_manifest_t *manifest_out, bool force)
{
    char url[384];
    snprintf(url, sizeof(url), "%s/api/v1/ota/check?current_version=%s",
             CONFIG_BACKEND_URL, CONFIG_FIRMWARE_VERSION);

    char response[2048];
    int status = 0;
    esp_err_t err = http_mtls_get(url, response, sizeof(response), &status);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *available = cJSON_GetObjectItem(root, "update_available");
    if (!cJSON_IsTrue(available)) {
        ESP_LOGI(TAG, "No OTA update available");
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    err = parse_manifest_from_json(root, manifest_out);
    cJSON_Delete(root);
    return err;
}

typedef struct {
    ota_manifest_t *manifest;
    mbedtls_sha256_context *sha;
    esp_ota_handle_t ota_handle;
    const esp_partition_t *part;
    bool header_ok;
    bool wrote_bytes;
    size_t total_written;
} ota_download_ctx_t;

static bool ota_header_gate_cb(const char *name, const char *value, void *ctx)
{
    ota_download_ctx_t *dctx = (ota_download_ctx_t *)ctx;
    if (name == NULL || value == NULL || dctx == NULL || dctx->manifest == NULL) {
        return true;
    }

    if (strcasecmp(name, "X-Firmware-Version") == 0 ||
        strcasecmp(name, "x-amz-meta-firmware-version") == 0) {
        if (strcmp(value, dctx->manifest->version) != 0) {
            ESP_LOGE(TAG, "Header version mismatch: got '%s', expected '%s'", value, dctx->manifest->version);
            dctx->header_ok = false;
            return false;
        }
        dctx->header_ok = true;
    }
    return true;
}

static esp_err_t ota_body_chunk_cb(const uint8_t *data, size_t len, void *ctx)
{
    ota_download_ctx_t *dctx = (ota_download_ctx_t *)ctx;
    if (dctx == NULL || data == NULL || len == 0) {
        return ESP_OK;
    }

    if (!dctx->wrote_bytes) {
        esp_err_t err = record_download_timestamp();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to record download timestamp: %s", esp_err_to_name(err));
        }
        dctx->wrote_bytes = true;
    }

    mbedtls_sha256_update(dctx->sha, data, len);
    esp_err_t err = esp_ota_write(dctx->ota_handle, data, len);
    if (err != ESP_OK) {
        return err;
    }
    dctx->total_written += len;
    return ESP_OK;
}

static esp_err_t ota_handler_apply_update(const ota_manifest_t *manifest)
{
    if (manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA download for version %s", manifest->version);

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        ESP_LOGE(TAG, "No OTA update partition");
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    unsigned char hash[32];
    char hash_hex[65];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    ota_download_ctx_t dctx = {
        .manifest = (ota_manifest_t *)manifest,
        .sha = &sha_ctx,
        .ota_handle = ota_handle,
        .part = update_part,
        .header_ok = false,
    };

    int status = 0;
    err = http_download_stream(manifest->download_url, ota_header_gate_cb, ota_body_chunk_cb, &dctx, &status);
    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        return err;
    }

    if (!dctx.header_ok) {
        ESP_LOGE(TAG, "Missing or invalid firmware version header");
        esp_ota_abort(ota_handle);
        mbedtls_sha256_free(&sha_ctx);
        return ESP_ERR_INVALID_RESPONSE;
    }

    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    for (int i = 0; i < 32; i++) {
        sprintf(hash_hex + (i * 2), "%02x", hash[i]);
    }
    hash_hex[64] = '\0';

    if (strcasecmp(hash_hex, manifest->sha256) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch");
        esp_ota_abort(ota_handle);
        return ESP_ERR_INVALID_CRC;
    }

    err = ota_signing_verify(manifest->sha256, manifest->signature);
    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA install complete (%u bytes), rebooting...", (unsigned)dctx.total_written);
    mqtt_handler_clear_retained_cmd();
    esp_restart();
    return ESP_OK;
}

static void ota_publish_progress(const char *version, int percent)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return;
    }
    cJSON_AddStringToObject(obj, "type", "ota_progress");
    cJSON_AddStringToObject(obj, "version", version);
    cJSON_AddNumberToObject(obj, "percent", percent);
    publish_status_json(obj);
    cJSON_Delete(obj);
}

static void ota_task_worker(void *arg)
{
    (void)arg;
    ota_work_item_t work = {0};

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_ota_events, OTA_WORK_CHECK | OTA_WORK_UPDATE,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (s_pending_verify_mode) {
            ESP_LOGW(TAG, "OTA worker idle — pending verify active");
            continue;
        }
        bool force = s_force_download;
        s_force_download = false;

        if ((bits & OTA_WORK_UPDATE) && s_pending_manifest.version[0] != '\0') {
            work.manifest = s_pending_manifest;
            work.force = force;
        } else {
            memset(&work, 0, sizeof(work));
            work.force = force;
        }

        if (!work.force && cooldown_active()) {
            ESP_LOGI(TAG, "OTA download skipped (cooldown active)");
            continue;
        }

        ota_manifest_t manifest = {0};
        esp_err_t err;
        if (work.manifest.version[0] != '\0') {
            manifest = work.manifest;
            err = ESP_OK;
        } else {
            err = ota_handler_check_for_update(&manifest, work.force);
        }

        if (err != ESP_OK) {
            continue;
        }

        ota_publish_progress(manifest.version, 0);
        err = ota_handler_apply_update(&manifest);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
            s_inflight_version[0] = '\0';
        }
    }
}

static bool version_already_running(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return false;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    return app != NULL && strcmp(app->version, version) == 0;
}

static void queue_ota_check(bool force)
{
    if (s_ota_events == NULL) {
        ESP_LOGW(TAG, "OTA check ignored — handler not initialized");
        return;
    }
    if (s_pending_verify_mode) {
        ESP_LOGW(TAG, "OTA check ignored — pending verify active");
        return;
    }
    s_force_download = force;
    xEventGroupSetBits(s_ota_events, OTA_WORK_CHECK);
    ESP_LOGI(TAG, "Queued ota_check (force=%d)", force ? 1 : 0);
}

static void queue_ota_update(const ota_manifest_t *manifest, bool force)
{
    if (s_ota_events == NULL || manifest == NULL) {
        ESP_LOGW(TAG, "OTA update ignored — handler not ready");
        return;
    }
    if (s_pending_verify_mode) {
        ESP_LOGW(TAG, "OTA update ignored — pending verify active");
        return;
    }
    if (!force && version_already_running(manifest->version)) {
        ESP_LOGI(TAG, "Already running version %s — clearing stale cmd", manifest->version);
        mqtt_handler_clear_retained_cmd();
        return;
    }
    if (s_inflight_version[0] != '\0' && strcmp(s_inflight_version, manifest->version) == 0) {
        ESP_LOGI(TAG, "OTA update for %s already in progress", manifest->version);
        return;
    }
    strncpy(s_inflight_version, manifest->version, sizeof(s_inflight_version) - 1);
    s_inflight_version[sizeof(s_inflight_version) - 1] = '\0';
    s_pending_manifest = *manifest;
    s_force_download = force;
    xEventGroupSetBits(s_ota_events, OTA_WORK_UPDATE);
    ESP_LOGI(TAG, "Queued ota_update version=%s force=%d", manifest->version, force ? 1 : 0);
}

static void poll_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OTA poll timer fired");
    queue_ota_check(false);
}

static uint64_t poll_interval_us(void)
{
#if CONFIG_OTA_POLL_INTERVAL_SEC > 0
    return (uint64_t)CONFIG_OTA_POLL_INTERVAL_SEC * 1000000ULL;
#else
    return (uint64_t)CONFIG_OTA_POLL_INTERVAL_HOURS * 3600ULL * 1000000ULL;
#endif
}

esp_err_t ota_handler_init(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';

    esp_err_t err = ota_signing_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA signing init: %s (verify will fail until key configured)", esp_err_to_name(err));
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify_mode = true;
        const esp_app_desc_t *app = esp_app_get_description();
        if (app != NULL) {
            strncpy(s_pending_version, app->version, sizeof(s_pending_version) - 1);
        }
        ESP_LOGW(TAG, "Booted with pending OTA verify for version %s", s_pending_version);
    }

    s_ota_events = xEventGroupCreate();
    s_rollback_ack_events = xEventGroupCreate();
    if (s_ota_events == NULL || s_rollback_ack_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t ota_handler_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ota_task == NULL) {
        if (xTaskCreate(ota_task_worker, "ota_worker", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, &s_ota_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_poll_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = poll_timer_cb,
            .name = "ota_poll",
        };
        esp_err_t err = esp_timer_create(&timer_args, &s_poll_timer);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_timer_start_periodic(s_poll_timer, poll_interval_us());
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "OTA poll timer started");
    }

    return ESP_OK;
}

void ota_handler_on_mqtt_cmd(const char *topic, const char *payload, int len)
{
    if (payload == NULL || len <= 0) {
        return;
    }

    char buf[2048];
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "MQTT cmd on %s: %.*s", topic ? topic : "?", len, buf);

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse OTA cmd JSON");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cmd = cJSON_GetObjectItem(root, "type");
    }
    if (!cJSON_IsString(cmd)) {
        ESP_LOGW(TAG, "OTA cmd missing \"cmd\" field");
        cJSON_Delete(root);
        return;
    }

    bool force = cJSON_IsTrue(cJSON_GetObjectItem(root, "force"));
    ESP_LOGI(TAG, "OTA command: %s", cmd->valuestring);

    if (strcmp(cmd->valuestring, "ota_check") == 0) {
        queue_ota_check(force);
    } else if (strcmp(cmd->valuestring, "ota_update") == 0) {
        ota_manifest_t manifest = {0};
        if (parse_manifest_from_json(root, &manifest) == ESP_OK) {
            queue_ota_update(&manifest, force);
        } else {
            ESP_LOGW(TAG, "ota_update missing manifest fields — falling back to ota_check");
            queue_ota_check(force);
        }
    } else {
        ESP_LOGD(TAG, "Ignoring non-OTA cmd: %s", cmd->valuestring);
    }

    cJSON_Delete(root);
}

void ota_handler_on_mqtt_ack(const char *topic, const char *payload, int len)
{
    if (payload == NULL || len <= 0 || s_rollback_ack_events == NULL) {
        return;
    }

    char buf[512];
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ota_rollback_received") == 0) {
        xEventGroupSetBits(s_rollback_ack_events, OTA_ROLLBACK_ACK_BIT);
    }

    cJSON_Delete(root);
    (void)topic;
}

void ota_handler_on_mqtt_connected(void)
{
    if (s_pending_verify_mode) {
        cJSON *obj = cJSON_CreateObject();
        if (obj != NULL) {
            cJSON_AddStringToObject(obj, "type", "ota_validating");
            cJSON_AddStringToObject(obj, "version", s_pending_version);
            publish_status_json(obj);
            cJSON_Delete(obj);
        }
    }
}

static void publish_rollback_and_wait(const char *version, const char *reason)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            break;
        }
        cJSON_AddStringToObject(obj, "type", "ota_rollback");
        cJSON_AddStringToObject(obj, "attempted_version", version);
        cJSON_AddStringToObject(obj, "reason", reason);
        publish_status_json(obj);
        cJSON_Delete(obj);

        EventBits_t bits = xEventGroupWaitBits(s_rollback_ack_events, OTA_ROLLBACK_ACK_BIT,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        if (bits & OTA_ROLLBACK_ACK_BIT) {
            ESP_LOGI(TAG, "Rollback ack received");
            return;
        }
    }
    ESP_LOGW(TAG, "Rollback ack not received after retries");
}

bool ota_handler_pending_verify_active(void)
{
    return s_pending_verify_mode;
}

void ota_handler_notify_wifi_mqtt_ready(void)
{
    if (!s_pending_verify_mode) {
        return;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
        publish_rollback_and_wait(s_pending_version, "mark_valid_failed");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }

    s_pending_verify_mode = false;
    mqtt_handler_clear_retained_cmd();

    cJSON *obj = cJSON_CreateObject();
    if (obj != NULL) {
        cJSON_AddStringToObject(obj, "type", "ota_success");
        cJSON_AddStringToObject(obj, "version", s_pending_version);
        publish_status_json(obj);
        cJSON_Delete(obj);
    }
    ESP_LOGI(TAG, "OTA pending verify succeeded for version %s", s_pending_version);
}

esp_err_t ota_handler_run_pending_verify(void)
{
    if (!s_pending_verify_mode) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Waiting for WiFi + MQTT before OTA validation...");
    for (int i = 0; i < 120; i++) {
        if (mqtt_handler_is_connected()) {
            ota_handler_notify_wifi_mqtt_ready();
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "OTA pending verify timeout");
    publish_rollback_and_wait(s_pending_version, "pending_verify_failed");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return ESP_FAIL;
}
