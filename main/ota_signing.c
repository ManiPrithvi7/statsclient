#include "ota_signing.h"

#include <ctype.h>
#include <string.h>

#include "ed25519/ed25519.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "ota_signing";

static unsigned char s_pubkey[32];
static bool s_pubkey_loaded;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t hex_to_bytes(const char *hex, unsigned char *out, size_t out_len)
{
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return ESP_OK;
}

esp_err_t ota_signing_init(void)
{
    if (s_pubkey_loaded) {
        return ESP_OK;
    }

    const char *hex = CONFIG_OTA_ED25519_PUBLIC_KEY_HEX;
    if (hex == NULL || strlen(hex) == 0) {
        ESP_LOGW(TAG, "OTA Ed25519 public key not configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = hex_to_bytes(hex, s_pubkey, sizeof(s_pubkey));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid OTA Ed25519 public key hex");
        return err;
    }

    s_pubkey_loaded = true;
    ESP_LOGI(TAG, "OTA signing public key loaded");
    return ESP_OK;
}

esp_err_t ota_signing_verify(const char *sha256_hex, const char *signature_b64)
{
    if (!s_pubkey_loaded) {
        esp_err_t init_err = ota_signing_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    if (sha256_hex == NULL || signature_b64 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(sha256_hex) != 64) {
        ESP_LOGE(TAG, "SHA-256 hex must be 64 characters");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)sha256_hex[i])) {
            ESP_LOGE(TAG, "SHA-256 hex contains non-hex characters");
            return ESP_ERR_INVALID_ARG;
        }
    }

    unsigned char signature[64];
    size_t sig_len = 0;
    int rc = mbedtls_base64_decode(signature, sizeof(signature), &sig_len,
                                   (const unsigned char *)signature_b64, strlen(signature_b64));
    if (rc != 0 || sig_len != sizeof(signature)) {
        ESP_LOGE(TAG, "Invalid base64 signature");
        return ESP_ERR_INVALID_ARG;
    }

    /* Server signs UTF-8 bytes of lowercase hex digest (sign-firmware.ts default). */
    if (!ed25519_verify(signature, (const unsigned char *)sha256_hex, 64, s_pubkey)) {
        ESP_LOGE(TAG, "Ed25519 signature verification failed");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "Firmware signature verified");
    return ESP_OK;
}
