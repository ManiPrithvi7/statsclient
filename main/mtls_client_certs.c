#include "mtls_client_certs.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "mtls_certs";

#if CONFIG_USE_EMBEDDED_MTLS_CERTS

extern const uint8_t _binary_client_crt_start[] asm("_binary_client_crt_start");
extern const uint8_t _binary_client_crt_end[] asm("_binary_client_crt_end");
extern const uint8_t _binary_client_key_start[] asm("_binary_client_key_start");
extern const uint8_t _binary_client_key_end[] asm("_binary_client_key_end");
extern const uint8_t _binary_ca_root_pem_start[] asm("_binary_ca_root_pem_start");
extern const uint8_t _binary_ca_root_pem_end[] asm("_binary_ca_root_pem_end");

static esp_err_t copy_embedded_pem(const uint8_t *start, const uint8_t *end, char *buf, size_t buf_size)
{
    if (start == NULL || end == NULL || end <= start || buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = (size_t)(end - start);
    if (len >= buf_size) {
        ESP_LOGE(TAG, "PEM buffer too small (%u needed, %u available)", (unsigned)len + 1, (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf, start, len);
    buf[len] = '\0';
    return ESP_OK;
}

bool mtls_client_certs_available(void)
{
    return true;
}

esp_err_t mtls_client_certs_load_device_cert(char *buf, size_t buf_size)
{
    return copy_embedded_pem(_binary_client_crt_start,
                             _binary_client_crt_end,
                             buf, buf_size);
}

esp_err_t mtls_client_certs_load_ca_cert(char *buf, size_t buf_size)
{
    return copy_embedded_pem(_binary_ca_root_pem_start,
                             _binary_ca_root_pem_end,
                             buf, buf_size);
}

const char *mtls_client_certs_get_private_key(void)
{
    static char key_buf[4096];
    static bool loaded;

    if (!loaded) {
        if (copy_embedded_pem(_binary_client_key_start,
                              _binary_client_key_end,
                              key_buf, sizeof(key_buf)) != ESP_OK) {
            return NULL;
        }
        loaded = true;
    }
    return key_buf;
}

#else

bool mtls_client_certs_available(void)
{
    return false;
}

esp_err_t mtls_client_certs_load_device_cert(char *buf, size_t buf_size)
{
    (void)buf;
    (void)buf_size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mtls_client_certs_load_ca_cert(char *buf, size_t buf_size)
{
    (void)buf;
    (void)buf_size;
    return ESP_ERR_NOT_SUPPORTED;
}

const char *mtls_client_certs_get_private_key(void)
{
    return NULL;
}

#endif
