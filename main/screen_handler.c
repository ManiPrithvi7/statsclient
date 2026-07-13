#include "screen_handler.h"

#include "dwin_protocol.h"
#include "screen_config.h"
#include "screen_data.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "screen_handler";

typedef enum {
    RUNTIME_CMD_RENDER = 0,
    RUNTIME_CMD_TOGGLE_CAROUSEL,
} runtime_cmd_t;

typedef enum {
    CAROUSEL_PAGE_GOOGLE = 0,
    CAROUSEL_PAGE_INSTAGRAM,
} carousel_page_t;

#if CONFIG_ENABLE_DWIN_DISPLAY

static SemaphoreHandle_t s_model_mutex;
static QueueHandle_t s_runtime_queue;
static TaskHandle_t s_screen_task;
static TaskHandle_t s_carousel_task;
static bool s_runtime_started;
static carousel_page_t s_active_page = CAROUSEL_PAGE_GOOGLE;
static google_screen_data_t s_google_data;
static instagram_screen_data_t s_instagram_data;
static bool s_logged_google_defaults;
static bool s_logged_instagram_defaults;

static const char *topic_last_segment(const char *topic)
{
    if (topic == NULL) {
        return "";
    }
    const char *slash = strrchr(topic, '/');
    return slash ? slash + 1 : topic;
}

static bool topic_leaf_is_google(const char *leaf)
{
    return leaf != NULL &&
           (strcmp(leaf, SCREEN_TOPIC_LEAF_TEST_GMB) == 0 ||
            strcmp(leaf, SCREEN_TOPIC_LEAF_GMB) == 0);
}

static bool topic_leaf_is_instagram(const char *leaf)
{
    return leaf != NULL && strcmp(leaf, SCREEN_TOPIC_LEAF_INSTAGRAM) == 0;
}

static uint8_t clamp_percent(cJSON *item)
{
    if (item == NULL) {
        return 0;
    }
    double value = 0.0;
    if (cJSON_IsNumber(item)) {
        value = item->valuedouble;
    } else if (cJSON_IsString(item) && item->valuestring != NULL) {
        value = strtod(item->valuestring, NULL);
    } else {
        return 0;
    }
    if (value < 0.0) {
        return 0;
    }
    if (value > 100.0) {
        return 100;
    }
    return (uint8_t)value;
}

static double json_number(cJSON *item)
{
    if (item == NULL) {
        return 0.0;
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return strtod(item->valuestring, NULL);
    }
    return 0.0;
}

static void copy_qr_text(cJSON *item, char *dest, size_t dest_size)
{
    if (dest == NULL || dest_size == 0) {
        return;
    }
    dest[0] = '\0';
    if (item != NULL && cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dest, item->valuestring, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

static bool get_envelope_payload(const char *leaf, cJSON *root, cJSON **payload_out)
{
    if (payload_out == NULL || root == NULL || !cJSON_IsObject(root)) {
        return false;
    }

    const char *wrap_key = NULL;
    if (topic_leaf_is_google(leaf)) {
        wrap_key = "google";
    } else if (topic_leaf_is_instagram(leaf)) {
        wrap_key = "instagram";
    }

    if (wrap_key != NULL) {
        cJSON *wrapped = cJSON_GetObjectItem(root, wrap_key);
        if (cJSON_IsObject(wrapped)) {
            cJSON *inner = cJSON_GetObjectItem(wrapped, "payload");
            if (cJSON_IsObject(inner)) {
                *payload_out = inner;
                return true;
            }
            if (wrapped->child != NULL) {
                *payload_out = wrapped;
                return true;
            }
        }
    }

    cJSON *flat = cJSON_GetObjectItem(root, "payload");
    if (cJSON_IsObject(flat)) {
        *payload_out = flat;
        return true;
    }

    if (root->child != NULL) {
        *payload_out = root;
        return true;
    }

    return false;
}

static bool build_screen_update(const char *topic, const char *payload_json, screen_update_t *out)
{
    if (out == NULL || topic == NULL || payload_json == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const char *leaf = topic_last_segment(topic);
    const bool is_google = topic_leaf_is_google(leaf);
    const bool is_instagram = topic_leaf_is_instagram(leaf);
    if (!is_google && !is_instagram) {
        return false;
    }

    cJSON *root = cJSON_Parse(payload_json);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *payload = NULL;
    if (!get_envelope_payload(leaf, root, &payload) || !cJSON_IsObject(payload)) {
        cJSON_Delete(root);
        return false;
    }

    if (is_google) {
        out->kind = SCREEN_UPDATE_GOOGLE;
        out->google.valid = true;
        out->google.verified_review = (uint32_t)json_number(cJSON_GetObjectItem(payload, "verifiedReview"));
        out->google.rating = (float)json_number(cJSON_GetObjectItem(payload, "rating"));
        out->google.remaining_goal = (int32_t)json_number(cJSON_GetObjectItem(payload, "remainingGoal"));
        out->google.next_goal = (uint32_t)json_number(cJSON_GetObjectItem(payload, "nextGoal"));
        out->google.progress = clamp_percent(cJSON_GetObjectItem(payload, "progress"));
        copy_qr_text(cJSON_GetObjectItem(payload, "qrText"), out->google.qr_text, sizeof(out->google.qr_text));
        cJSON_Delete(root);
        return true;
    }

    out->kind = SCREEN_UPDATE_INSTAGRAM;
    out->instagram.valid = true;
    out->instagram.followers = (uint32_t)json_number(cJSON_GetObjectItem(payload, "followers"));
    cJSON *next_goal = cJSON_GetObjectItem(payload, "nextGoal");
    if (next_goal != NULL) {
        out->instagram.next_goal = (uint32_t)json_number(next_goal);
    } else {
        out->instagram.next_goal = (uint32_t)json_number(cJSON_GetObjectItem(payload, "achievement"));
    }
    out->instagram.remaining_goal = (int32_t)json_number(cJSON_GetObjectItem(payload, "remainingGoal"));
    out->instagram.progress = clamp_percent(cJSON_GetObjectItem(payload, "progress"));
    copy_qr_text(cJSON_GetObjectItem(payload, "qrText"), out->instagram.qr_text, sizeof(out->instagram.qr_text));
    cJSON_Delete(root);
    return true;
}

static uint16_t clamp_u16(long value, uint16_t max_value)
{
    if (value < 0) {
        return 0;
    }
    if (value > (long)max_value) {
        return max_value;
    }
    return (uint16_t)value;
}

static void write_three_digits(uint16_t vp100, uint16_t vp10, uint16_t vp1, uint16_t value)
{
    dwin_write_word(vp100, (uint16_t)((value / 100U) % 10U));
    dwin_write_word(vp10, (uint16_t)((value / 10U) % 10U));
    dwin_write_word(vp1, (uint16_t)(value % 10U));
}

static void write_two_digits(uint16_t vp10, uint16_t vp1, uint16_t value)
{
    dwin_write_word(vp10, (uint16_t)((value / 10U) % 10U));
    dwin_write_word(vp1, (uint16_t)(value % 10U));
}

static void write_six_digits(uint16_t vp100000, uint16_t vp10000, uint16_t vp1000,
                             uint16_t vp100, uint16_t vp10, uint16_t vp1, uint32_t value)
{
    if (value > 999999U) {
        value = 999999U;
    }
    dwin_write_word(vp100000, (uint16_t)((value / 100000U) % 10U));
    dwin_write_word(vp10000, (uint16_t)((value / 10000U) % 10U));
    dwin_write_word(vp1000, (uint16_t)((value / 1000U) % 10U));
    dwin_write_word(vp100, (uint16_t)((value / 100U) % 10U));
    dwin_write_word(vp10, (uint16_t)((value / 10U) % 10U));
    dwin_write_word(vp1, (uint16_t)(value % 10U));
}

static uint16_t google_progress_icon_for_percent(uint8_t percent)
{
    if (percent >= 100U) {
        return 61U;
    }
    if (percent >= 80U) {
        return 60U;
    }
    if (percent >= 60U) {
        return 59U;
    }
    if (percent >= 40U) {
        return 58U;
    }
    if (percent >= 20U) {
        return 57U;
    }
    return 56U;
}

static void render_google_page(const google_screen_data_t *google)
{
    const uint16_t verified = clamp_u16((long)google->verified_review, 999U);
    const uint16_t next_goal = clamp_u16((long)google->next_goal, 999U);
    const uint16_t remaining = clamp_u16((long)google->remaining_goal, 9U);
    long rating_tenths_long = (long)lroundf(google->rating * 10.0f);
    if (rating_tenths_long < 0) {
        rating_tenths_long = 0;
    }
    if (rating_tenths_long > 99) {
        rating_tenths_long = 99;
    }
    const uint16_t rating_tenths = (uint16_t)rating_tenths_long;

    write_three_digits(SCREEN_VP_GOOGLE_VERIFIED_REVIEWS_100,
                       SCREEN_VP_GOOGLE_VERIFIED_REVIEWS_10,
                       SCREEN_VP_GOOGLE_VERIFIED_REVIEWS_1,
                       verified);
    dwin_write_word(SCREEN_VP_GOOGLE_REMAIN_GOAL, remaining);
    write_three_digits(SCREEN_VP_GOOGLE_NEXT_GOAL_100,
                       SCREEN_VP_GOOGLE_NEXT_GOAL_10,
                       SCREEN_VP_GOOGLE_NEXT_GOAL_1,
                       next_goal);
    dwin_write_word(SCREEN_VP_GOOGLE_PROGRESS_BAR, google_progress_icon_for_percent(google->progress));
    dwin_write_word(SCREEN_VP_GOOGLE_RATING_TENTHS, (uint16_t)((rating_tenths / 10U) % 10U));
    dwin_write_word(SCREEN_VP_GOOGLE_RATING_DOT, 50U);
    dwin_write_word(SCREEN_VP_GOOGLE_RATING_DECIMAL, (uint16_t)(rating_tenths % 10U));
    if (google->qr_text[0] != '\0') {
        dwin_send_qr(SCREEN_VP_GOOGLE_QR, google->qr_text, SCREEN_GOOGLE_QR_MAX_BYTES);
    }
}

static void render_instagram_page(const instagram_screen_data_t *instagram)
{
    const uint16_t remaining = clamp_u16((long)instagram->remaining_goal, 99U);
    uint16_t progress_state = (uint16_t)(instagram->progress / 2U);
    if (progress_state > 50U) {
        progress_state = 50U;
    }

    write_six_digits(SCREEN_VP_INSTAGRAM_FOLLOWERS_100000,
                     SCREEN_VP_INSTAGRAM_FOLLOWERS_10000,
                     SCREEN_VP_INSTAGRAM_FOLLOWERS_1000,
                     SCREEN_VP_INSTAGRAM_FOLLOWERS_100,
                     SCREEN_VP_INSTAGRAM_FOLLOWERS_10,
                     SCREEN_VP_INSTAGRAM_FOLLOWERS_1,
                     instagram->followers);
    write_six_digits(SCREEN_VP_INSTAGRAM_ACHIEVEMENTS_100000,
                     SCREEN_VP_INSTAGRAM_ACHIEVEMENTS_10000,
                     SCREEN_VP_INSTAGRAM_ACHIEVEMENTS_1000,
                     SCREEN_VP_INSTAGRAM_ACHIEVEMENTS_100,
                     SCREEN_VP_INSTAGRAM_ACHIEVEMENTS_10,
                     SCREEN_VP_INSTAGRAM_ACHIEVEMENTS_1,
                     instagram->next_goal);
    write_two_digits(SCREEN_VP_INSTAGRAM_REMAIN_GOAL_10,
                     SCREEN_VP_INSTAGRAM_REMAIN_GOAL_1,
                     remaining);
    dwin_write_word(SCREEN_VP_INSTAGRAM_PROGRESS_BAR, progress_state);
    if (instagram->qr_text[0] != '\0') {
        dwin_send_qr(SCREEN_VP_INSTAGRAM_QR, instagram->qr_text, SCREEN_INSTAGRAM_QR_MAX_BYTES);
    }
}

static void apply_screen_update(const screen_update_t *update)
{
    if (s_model_mutex == NULL || update == NULL) {
        return;
    }
    if (xSemaphoreTake(s_model_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    if (update->kind == SCREEN_UPDATE_GOOGLE) {
        s_google_data = update->google;
    } else if (update->kind == SCREEN_UPDATE_INSTAGRAM) {
        s_instagram_data = update->instagram;
    }

    xSemaphoreGive(s_model_mutex);
}

static void request_screen_render(void)
{
    if (!s_runtime_started || s_runtime_queue == NULL) {
        return;
    }
    const runtime_cmd_t cmd = RUNTIME_CMD_RENDER;
    xQueueSend(s_runtime_queue, &cmd, 0);
}

static void render_current_page(void)
{
    if (s_model_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_model_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    const carousel_page_t page = s_active_page;
    const google_screen_data_t google = s_google_data;
    const instagram_screen_data_t instagram = s_instagram_data;

    xSemaphoreGive(s_model_mutex);

    if (page == CAROUSEL_PAGE_GOOGLE) {
        dwin_set_page(SCREEN_PAGE_GOOGLE);
        render_google_page(&google);
        if (!google.valid && !s_logged_google_defaults) {
            ESP_LOGI(TAG, "page=google no MQTT data yet; rendering defaults");
            s_logged_google_defaults = true;
        }
        ESP_LOGI(TAG, "page=google verifiedReview=%lu rating=%.2f remainingGoal=%ld nextGoal=%lu progress=%u qrLen=%u",
                 (unsigned long)google.verified_review, (double)google.rating,
                 (long)google.remaining_goal, (unsigned long)google.next_goal,
                 (unsigned)google.progress, (unsigned)strlen(google.qr_text));
        return;
    }

    dwin_set_page(SCREEN_PAGE_INSTAGRAM);
    render_instagram_page(&instagram);
    if (!instagram.valid && !s_logged_instagram_defaults) {
        ESP_LOGI(TAG, "page=instagram no MQTT data yet; rendering defaults");
        s_logged_instagram_defaults = true;
    }
    ESP_LOGI(TAG, "page=instagram followers=%lu nextGoal=%lu remainingGoal=%ld progress=%u qrLen=%u",
             (unsigned long)instagram.followers, (unsigned long)instagram.next_goal,
             (long)instagram.remaining_goal, (unsigned)instagram.progress,
             (unsigned)strlen(instagram.qr_text));
}

static void screen_task(void *arg)
{
    (void)arg;
    runtime_cmd_t cmd = RUNTIME_CMD_RENDER;

    for (;;) {
        if (s_runtime_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (xQueueReceive(s_runtime_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd == RUNTIME_CMD_TOGGLE_CAROUSEL) {
                if (s_model_mutex != NULL &&
                    xSemaphoreTake(s_model_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    s_active_page = (s_active_page == CAROUSEL_PAGE_GOOGLE)
                                        ? CAROUSEL_PAGE_INSTAGRAM
                                        : CAROUSEL_PAGE_GOOGLE;
                    xSemaphoreGive(s_model_mutex);
                }
            }
            render_current_page();
        }
    }
}

static void carousel_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_DWIN_CAROUSEL_INTERVAL_MS));
        if (!s_runtime_started || s_runtime_queue == NULL) {
            continue;
        }
        const runtime_cmd_t cmd = RUNTIME_CMD_TOGGLE_CAROUSEL;
        xQueueSend(s_runtime_queue, &cmd, 0);
    }
}

static void stop_runtime_tasks(void)
{
    if (s_carousel_task != NULL) {
        vTaskDelete(s_carousel_task);
        s_carousel_task = NULL;
    }
    if (s_screen_task != NULL) {
        vTaskDelete(s_screen_task);
        s_screen_task = NULL;
    }
    if (s_runtime_queue != NULL) {
        xQueueReset(s_runtime_queue);
    }
    s_runtime_started = false;
}

esp_err_t screen_handler_init(void)
{
    esp_err_t err = dwin_init();
    if (err != ESP_OK) {
        return err;
    }

    if (s_model_mutex == NULL) {
        s_model_mutex = xSemaphoreCreateMutex();
        if (s_model_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_runtime_queue == NULL) {
        s_runtime_queue = xQueueCreate(16, sizeof(runtime_cmd_t));
        if (s_runtime_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

void screen_handler_start(void)
{
    if (s_runtime_started) {
        return;
    }

    if (s_model_mutex == NULL || s_runtime_queue == NULL) {
        ESP_LOGE(TAG, "screen handler not initialized");
        return;
    }

    BaseType_t screen_ok = pdPASS;
    BaseType_t carousel_ok = pdPASS;

    if (s_screen_task == NULL) {
        screen_ok = xTaskCreate(screen_task, "screen_task", 6144, NULL, 2, &s_screen_task);
    }
    if (s_carousel_task == NULL) {
        carousel_ok = xTaskCreate(carousel_task, "carousel_task", 3072, NULL, 1, &s_carousel_task);
    }

    if (screen_ok != pdPASS || carousel_ok != pdPASS ||
        s_screen_task == NULL || s_carousel_task == NULL) {
        ESP_LOGE(TAG, "failed to start runtime tasks");
        stop_runtime_tasks();
        return;
    }

    s_runtime_started = true;
    s_logged_google_defaults = false;
    s_logged_instagram_defaults = false;
    ESP_LOGI(TAG, "screen + carousel tasks started");
    request_screen_render();
}

void screen_handler_stop(void)
{
    stop_runtime_tasks();
}

void screen_handler_on_mqtt(const char *topic, const char *payload, int len)
{
    if (topic == NULL || payload == NULL || len <= 0) {
        return;
    }

    if (len <= 0 || len >= SCREEN_JSON_BUF_SIZE || payload[len] != '\0') {
        ESP_LOGW(TAG, "screen payload invalid (len=%d) on %s", len, topic);
        return;
    }

    screen_update_t update;
    if (!build_screen_update(topic, payload, &update)) {
        ESP_LOGW(TAG, "screen parse failed topic=%s len=%d", topic, len);
        return;
    }

    if (update.kind == SCREEN_UPDATE_GOOGLE) {
        ESP_LOGI(TAG, "screen update google verifiedReview=%lu rating=%.2f nextGoal=%lu progress=%u",
                 (unsigned long)update.google.verified_review,
                 (double)update.google.rating,
                 (unsigned long)update.google.next_goal,
                 (unsigned)update.google.progress);
    } else if (update.kind == SCREEN_UPDATE_INSTAGRAM) {
        ESP_LOGI(TAG, "screen update instagram followers=%lu nextGoal=%lu progress=%u",
                 (unsigned long)update.instagram.followers,
                 (unsigned long)update.instagram.next_goal,
                 (unsigned)update.instagram.progress);
    }

    apply_screen_update(&update);
    request_screen_render();
}

#else /* !CONFIG_ENABLE_DWIN_DISPLAY */

esp_err_t screen_handler_init(void)
{
    return ESP_OK;
}

void screen_handler_start(void) {}
void screen_handler_stop(void) {}

void screen_handler_on_mqtt(const char *topic, const char *payload, int len)
{
    (void)topic;
    (void)payload;
    (void)len;
}

#endif /* CONFIG_ENABLE_DWIN_DISPLAY */
