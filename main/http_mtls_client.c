#include "http_mtls_client.h"

#include <strings.h>
#include <string.h>
#include <stdlib.h>

#include "certificate_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "http_mtls";

#define CERT_BUFFER_SIZE 4096
#define HTTP_DOWNLOAD_TIMEOUT_MS 120000

static char s_device_cert[CERT_BUFFER_SIZE];
static char s_ca_cert[CERT_BUFFER_SIZE];
static bool s_certs_loaded;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    http_header_cb_t header_cb;
    http_body_chunk_cb_t body_cb;
    void *user_ctx;
    bool header_gate_failed;
} http_stream_ctx_t;

static esp_err_t ensure_mtls_certs_loaded(void)
{
    if (s_certs_loaded) {
        return ESP_OK;
    }
    if (!certificate_manager_has_certificates()) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = certificate_manager_load_device_cert(s_device_cert, sizeof(s_device_cert));
    if (err != ESP_OK) {
        return err;
    }
    err = certificate_manager_load_ca_cert(s_ca_cert, sizeof(s_ca_cert));
    if (err != ESP_OK) {
        return err;
    }
    s_certs_loaded = true;
    return ESP_OK;
}

static const char *backend_host_from_config(void)
{
    static char host[128];
    const char *url = CONFIG_BACKEND_URL;
    if (url == NULL || url[0] == '\0') {
        return NULL;
    }

    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    size_t i = 0;
    while (start[i] != '\0' && start[i] != '/' && start[i] != ':' && i < sizeof(host) - 1) {
        host[i] = start[i];
        i++;
    }
    host[i] = '\0';
    return host[0] ? host : NULL;
}

bool http_url_uses_backend_mtls(const char *url)
{
    if (url == NULL) {
        return false;
    }

    const char *backend_host = backend_host_from_config();
    if (backend_host == NULL) {
        return true;
    }

    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;

    char url_host[128] = {0};
    size_t i = 0;
    while (start[i] != '\0' && start[i] != '/' && start[i] != ':' && i < sizeof(url_host) - 1) {
        url_host[i] = start[i];
        i++;
    }

    return strcasecmp(url_host, backend_host) == 0;
}

static esp_err_t configure_client(esp_http_client_config_t *cfg, const char *url, bool use_mtls)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = HTTP_DOWNLOAD_TIMEOUT_MS;
    cfg->buffer_size = 4096;
    cfg->buffer_size_tx = 1024;

    if (use_mtls) {
        esp_err_t err = ensure_mtls_certs_loaded();
        if (err != ESP_OK) {
            return err;
        }
        cfg->client_cert_pem = s_device_cert;
        cfg->client_key_pem = certificate_manager_get_private_key();
        /* Verify server TLS with public CA bundle; device CA is for client auth only. */
        cfg->crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        cfg->crt_bundle_attach = esp_crt_bundle_attach;
    }
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_stream_ctx_t *ctx = (http_stream_ctx_t *)evt->user_data;
    if (ctx == NULL) {
        return ESP_OK;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (ctx->header_cb != NULL) {
            if (!ctx->header_cb(evt->header_key, evt->header_value, ctx->user_ctx)) {
                ctx->header_gate_failed = true;
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (ctx->body_cb != NULL && evt->data_len > 0) {
            esp_err_t err = ctx->body_cb((const uint8_t *)evt->data, evt->data_len, ctx->user_ctx);
            if (err != ESP_OK) {
                return err;
            }
        } else if (ctx->buf != NULL && evt->data_len > 0) {
            if (ctx->len + evt->data_len >= ctx->cap) {
                ESP_LOGE(TAG, "Response buffer overflow");
                return ESP_ERR_NO_MEM;
            }
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
            ctx->buf[ctx->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t http_mtls_get(const char *url, char *response_buf, size_t buf_size, int *status_out)
{
    if (url == NULL || response_buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    http_stream_ctx_t stream = {
        .buf = response_buf,
        .cap = buf_size - 1,
        .len = 0,
    };
    response_buf[0] = '\0';

    esp_http_client_config_t cfg;
    esp_err_t err = configure_client(&cfg, url, http_url_uses_backend_mtls(url));
    if (err != ESP_OK) {
        return err;
    }
    cfg.event_handler = http_event_handler;
    cfg.user_data = &stream;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out != NULL) {
        *status_out = status;
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "GET HTTP status %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t http_download_stream(const char *url,
                               http_header_cb_t header_cb,
                               http_body_chunk_cb_t body_chunk_cb,
                               void *ctx,
                               int *status_out)
{
    if (url == NULL || body_chunk_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    http_stream_ctx_t stream = {
        .header_cb = header_cb,
        .body_cb = body_chunk_cb,
        .user_ctx = ctx,
    };

    esp_http_client_config_t cfg;
    esp_err_t err = configure_client(&cfg, url, http_url_uses_backend_mtls(url));
    if (err != ESP_OK) {
        return err;
    }
    cfg.event_handler = http_event_handler;
    cfg.user_data = &stream;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out != NULL) {
        *status_out = status;
    }
    esp_http_client_cleanup(client);

    if (stream.header_gate_failed) {
        ESP_LOGE(TAG, "Download aborted: header gate failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Download HTTP status %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}
