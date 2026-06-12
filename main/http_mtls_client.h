#ifndef HTTP_MTLS_CLIENT_H
#define HTTP_MTLS_CLIENT_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*http_header_cb_t)(const char *name, const char *value, void *ctx);
typedef esp_err_t (*http_body_chunk_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t http_mtls_get(const char *url, char *response_buf, size_t buf_size, int *status_out);

esp_err_t http_download_stream(const char *url,
                               http_header_cb_t header_cb,
                               http_body_chunk_cb_t body_chunk_cb,
                               void *ctx,
                               int *status_out);

bool http_url_uses_backend_mtls(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_MTLS_CLIENT_H */
