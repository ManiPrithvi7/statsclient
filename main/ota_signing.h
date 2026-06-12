#ifndef OTA_SIGNING_H
#define OTA_SIGNING_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Signing payload contract (must match proofmqtt/scripts/ota/sign-firmware.ts):
 *   message = UTF-8 bytes of lowercase SHA-256 hex digest (64 ASCII chars)
 *   signature = raw Ed25519 (64 bytes), base64-encoded in manifest JSON
 *
 * Public key: CONFIG_OTA_ED25519_PUBLIC_KEY_HEX (64 hex chars = 32-byte raw key)
 */
esp_err_t ota_signing_init(void);

esp_err_t ota_signing_verify(const char *sha256_hex, const char *signature_b64);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SIGNING_H */
