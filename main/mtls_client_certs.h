#ifndef MTLS_CLIENT_CERTS_H
#define MTLS_CLIENT_CERTS_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mtls_client_certs_available(void);

esp_err_t mtls_client_certs_load_device_cert(char *buf, size_t buf_size);
esp_err_t mtls_client_certs_load_ca_cert(char *buf, size_t buf_size);
const char *mtls_client_certs_get_private_key(void);

#ifdef __cplusplus
}
#endif

#endif
