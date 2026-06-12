/* MQTT Handler Implementation
 *
 * Handles mTLS MQTT connection to the broker.
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "mqtt_handler.h"
#include "certificate_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "mqtt_handler";

#define MQTT_BROKER_URI CONFIG_MQTT_BROKER_URI
#define CERT_BUFFER_SIZE 4096
#define TOPIC_BUF_SIZE 160
#define DEVICE_ID_BUF_SIZE 64

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;
static char s_device_cert[CERT_BUFFER_SIZE] = {0};
static char s_ca_cert[CERT_BUFFER_SIZE] = {0};
static char s_device_id[DEVICE_ID_BUF_SIZE] = {0};
static char s_cmd_topic[TOPIC_BUF_SIZE] = {0};
static char s_ack_topic[TOPIC_BUF_SIZE] = {0};
static char s_status_topic[TOPIC_BUF_SIZE] = {0};
static char s_broadcast_cmd_topic[TOPIC_BUF_SIZE] = {0};
static char s_device_wildcard_topic[TOPIC_BUF_SIZE] = {0};

static mqtt_cmd_callback_t s_cmd_callback;
static mqtt_cmd_callback_t s_ack_callback;
static mqtt_connected_callback_t s_connected_callback;

static esp_err_t load_device_id_from_nvs(char *device_id, size_t len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("device_config", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t required = len;
    err = nvs_get_str(nvs, "device_id", device_id, &required);
    nvs_close(nvs);
    return err;
}

static void mqtt_build_topics(void)
{
    if (s_device_id[0] == '\0') {
        return;
    }
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "%s/%s/cmd", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
    snprintf(s_ack_topic, sizeof(s_ack_topic), "%s/%s/ack", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
    snprintf(s_status_topic, sizeof(s_status_topic), "%s/%s/status", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
    snprintf(s_broadcast_cmd_topic, sizeof(s_broadcast_cmd_topic), "%s/broadcast/cmd", CONFIG_MQTT_TOPIC_ROOT);
    snprintf(s_device_wildcard_topic, sizeof(s_device_wildcard_topic), "%s/%s/#", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
}

static esp_err_t mqtt_subscribe_topic(const char *topic)
{
    esp_err_t err = mqtt_handler_subscribe(topic, 1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "subscribe %s: OK", topic);
    } else {
        ESP_LOGE(TAG, "subscribe %s failed", topic);
    }
    return err;
}

/** Match Node mtlsclient buildSubscribeTopics() in src/mqttClient.js */
static void mqtt_subscribe_lifecycle_topics(void)
{
    if (s_device_id[0] == '\0') {
        return;
    }

#if CONFIG_MQTT_SUBSCRIBE_ALL
    ESP_LOGI(TAG, "Subscribing under %s/%s/... (all=1)", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
    if (s_device_wildcard_topic[0] != '\0') {
        mqtt_subscribe_topic(s_device_wildcard_topic);
    }
    if (s_broadcast_cmd_topic[0] != '\0') {
        mqtt_subscribe_topic(s_broadcast_cmd_topic);
    }
#else
    ESP_LOGI(TAG, "Subscribing under %s/%s/... (all=0)", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
    static const char *const suffixes[] = {
        "registration_ack",
        "test-gmb",
        "instagram",
        "gmb",
        "pos",
        "promotion",
        "cmd",
        "ack",
        NULL,
    };

    char topic[TOPIC_BUF_SIZE];
    for (int i = 0; suffixes[i] != NULL; i++) {
        snprintf(topic, sizeof(topic), "%s/%s/%s", CONFIG_MQTT_TOPIC_ROOT, s_device_id, suffixes[i]);
        mqtt_subscribe_topic(topic);
    }
    if (s_broadcast_cmd_topic[0] != '\0') {
        mqtt_subscribe_topic(s_broadcast_cmd_topic);
    }
#endif
}

static bool topic_is_cmd_channel(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') {
        return false;
    }
    if (s_broadcast_cmd_topic[0] != '\0' && strcmp(topic, s_broadcast_cmd_topic) == 0) {
        return true;
    }
    size_t len = strlen(topic);
    if (len >= 4 && strcmp(topic + len - 4, "/cmd") == 0) {
        return true;
    }
    return false;
}

static bool topic_is_ack_channel(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') {
        return false;
    }
    size_t len = strlen(topic);
    return len >= 4 && strcmp(topic + len - 4, "/ack") == 0;
}

static void mqtt_publish_active(void)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return;
    }
    cJSON_AddStringToObject(obj, "type", "registration");
    cJSON_AddStringToObject(obj, "appVersion", CONFIG_FIRMWARE_VERSION);
    cJSON_AddStringToObject(obj, "clientId", s_device_id);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json == NULL) {
        return;
    }

    char topic[TOPIC_BUF_SIZE];
    snprintf(topic, sizeof(topic), "%s/%s/active", CONFIG_MQTT_TOPIC_ROOT, s_device_id);
    mqtt_handler_publish(topic, json, strlen(json), 1);
    cJSON_free(json);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED — mTLS handshake successful (client_id=%s)", s_device_id);
        s_mqtt_connected = true;
        mqtt_subscribe_lifecycle_topics();
        mqtt_publish_active();
        if (s_connected_callback != NULL) {
            s_connected_callback();
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA: {
        char topic[TOPIC_BUF_SIZE];
        int tlen = event->topic_len;
        if (tlen >= (int)sizeof(topic)) {
            tlen = (int)sizeof(topic) - 1;
        }
        memcpy(topic, event->topic, tlen);
        topic[tlen] = '\0';

        ESP_LOGI(TAG, "message topic=%s len=%d", topic, event->data_len);

        if (s_cmd_callback != NULL && topic_is_cmd_channel(topic)) {
            s_cmd_callback(topic, event->data, event->data_len);
        } else if (s_ack_callback != NULL && topic_is_ack_channel(topic)) {
            s_ack_callback(topic, event->data, event->data_len);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;

    default:
        break;
    }
}

void mqtt_handler_set_device_id(const char *device_id)
{
    if (device_id == NULL) {
        return;
    }
    strncpy(s_device_id, device_id, sizeof(s_device_id) - 1);
    s_device_id[sizeof(s_device_id) - 1] = '\0';
    mqtt_build_topics();
}

void mqtt_handler_set_cmd_callback(mqtt_cmd_callback_t cb)
{
    s_cmd_callback = cb;
}

void mqtt_handler_set_ack_callback(mqtt_cmd_callback_t cb)
{
    s_ack_callback = cb;
}

void mqtt_handler_set_connected_callback(mqtt_connected_callback_t cb)
{
    s_connected_callback = cb;
}

esp_err_t mqtt_handler_start(void)
{
    if (s_mqtt_client != NULL) {
        return ESP_OK;
    }

    if (s_device_id[0] == '\0') {
        esp_err_t id_err = load_device_id_from_nvs(s_device_id, sizeof(s_device_id));
        if (id_err != ESP_OK) {
#if CONFIG_USE_EMBEDDED_MTLS_CERTS
            strncpy(s_device_id, CONFIG_MTLS_CLIENT_DEVICE_ID, sizeof(s_device_id) - 1);
            s_device_id[sizeof(s_device_id) - 1] = '\0';
            ESP_LOGI(TAG, "Using embedded mTLS device_id: %s", s_device_id);
#else
            ESP_LOGE(TAG, "device_id not set and not found in NVS");
            return id_err;
#endif
        }
        mqtt_build_topics();
    }

    if (!certificate_manager_has_certificates()) {
        ESP_LOGE(TAG, "Certificates not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = certificate_manager_load_device_cert(s_device_cert, sizeof(s_device_cert));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = certificate_manager_load_ca_cert(s_ca_cert, sizeof(s_ca_cert));
    if (ret != ESP_OK) {
        return ret;
    }

    const char *private_key = certificate_manager_get_private_key();
    if (private_key == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
            .verification = {
                .certificate = s_ca_cert,
            },
        },
        .credentials = {
            .client_id = s_device_id,
            .authentication = {
                .certificate = s_device_cert,
                .key = private_key,
            },
        },
        .session = {
            .keepalive = 120,
            .disable_clean_session = true,
        },
        .network = {
            .reconnect_timeout_ms = 10000,
            .timeout_ms = 20000,
        },
    };

    ESP_LOGI(TAG, "Connecting to MQTT broker: %s", MQTT_BROKER_URI);
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ret;
    }

    return ESP_OK;
}

void mqtt_handler_stop(void)
{
    if (s_mqtt_client == NULL) {
        return;
    }
    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_mqtt_connected = false;
}

bool mqtt_handler_is_connected(void)
{
    return s_mqtt_connected;
}

esp_err_t mqtt_handler_publish(const char *topic, const char *data, int data_len, int qos)
{
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, data_len, qos, 0);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_handler_subscribe(const char *topic, int qos)
{
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, topic, qos);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_handler_publish_status_json(const char *json)
{
    if (json == NULL || s_status_topic[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return mqtt_handler_publish(s_status_topic, json, strlen(json), 1);
}

esp_err_t mqtt_handler_clear_retained_cmd(void)
{
    if (s_mqtt_client == NULL || !s_mqtt_connected || s_cmd_topic[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_cmd_topic, "", 0, 1, 1);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Cleared retained cmd on %s", s_cmd_topic);
    return ESP_OK;
}
