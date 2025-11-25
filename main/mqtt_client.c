/* MQTT Client Implementation
 *
 * Handles mTLS MQTT connection to the broker.
 */

#include <string.h>
#include "mqtt_client.h"
#include "certificate_manager.h"
#include "esp_log.h"
#include "mqtt_client.h"  // From esp-idf/components/mqtt/include
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "mqtt_client";

// Configuration from Kconfig
#define MQTT_BROKER_URI CONFIG_MQTT_BROKER_URI

// Certificate buffer sizes
#define CERT_BUFFER_SIZE 2048

// Global MQTT client handle
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

// Certificate buffers
static char s_device_cert[CERT_BUFFER_SIZE] = {0};
static char s_ca_cert[CERT_BUFFER_SIZE] = {0};

/**
 * @brief MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        s_mqtt_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        }
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

/**
 * @brief Start MQTT client with mTLS
 */
esp_err_t mqtt_client_start(void)
{
    if (s_mqtt_client != NULL) {
        ESP_LOGW(TAG, "MQTT client already started");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting MQTT client with mTLS");

    // Check if certificates exist
    if (!certificate_manager_has_certificates()) {
        ESP_LOGE(TAG, "Certificates not found. Cannot start MQTT client.");
        return ESP_ERR_NOT_FOUND;
    }

    // Load certificates from NVS
    esp_err_t ret = certificate_manager_load_device_cert(s_device_cert, sizeof(s_device_cert));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load device certificate: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = certificate_manager_load_ca_cert(s_ca_cert, sizeof(s_ca_cert));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load CA certificate: %s", esp_err_to_name(ret));
        return ret;
    }

    // Get private key
    const char *private_key = certificate_manager_get_private_key();
    if (private_key == NULL) {
        ESP_LOGE(TAG, "Failed to get private key");
        return ESP_ERR_NOT_FOUND;
    }

    // Configure MQTT client with mTLS
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
            .verification = {
                .certificate = s_ca_cert,  // CA cert to verify broker
            },
        },
        .credentials = {
            .authentication = {
                .certificate = s_device_cert,  // Client certificate
                .key = private_key,            // Client private key
            },
        },
    };

    ESP_LOGI(TAG, "Connecting to MQTT broker: %s", MQTT_BROKER_URI);

    // Initialize MQTT client
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_ERR_NO_MEM;
    }

    // Register event handler
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // Start MQTT client
    ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started successfully");
    return ESP_OK;
}

/**
 * @brief Stop MQTT client
 */
void mqtt_client_stop(void)
{
    if (s_mqtt_client == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Stopping MQTT client");
    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_mqtt_connected = false;
}

/**
 * @brief Check if MQTT client is connected
 */
bool mqtt_client_is_connected(void)
{
    return s_mqtt_connected;
}

/**
 * @brief Publish message to MQTT topic
 */
esp_err_t mqtt_client_publish(const char *topic, const char *data, int data_len, int qos)
{
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        ESP_LOGE(TAG, "MQTT client not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, data_len, qos, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published message to %s, msg_id=%d", topic, msg_id);
    return ESP_OK;
}

/**
 * @brief Subscribe to MQTT topic
 */
esp_err_t mqtt_client_subscribe(const char *topic, int qos)
{
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        ESP_LOGE(TAG, "MQTT client not connected");
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to topic");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", topic, msg_id);
    return ESP_OK;
}

