#include "prov_token_validate.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "prov_jwt";

/** Copy JWT payload segment (middle part) into out; returns length or 0 on error. */
static size_t jwt_copy_payload_segment(const char *jwt, char *out, size_t out_max)
{
    if (jwt == NULL || out == NULL || out_max < 8) {
        return 0;
    }

    const char *p1 = strchr(jwt, '.');
    if (!p1) {
        return 0;
    }
    const char *p2 = strchr(p1 + 1, '.');
    if (!p2) {
        return 0;
    }

    size_t enc_len = (size_t)(p2 - (p1 + 1));
    if (enc_len == 0 || enc_len >= out_max) {
        return 0;
    }

    memcpy(out, p1 + 1, enc_len);
    out[enc_len] = '\0';
    return enc_len;
}

/** base64url -> base64 in-place-ish: writes decoded binary to `dec`, returns length. */
static int base64url_decode(const char *b64url, unsigned char *dec, size_t dec_size)
{
    size_t n = strlen(b64url);
    char *tmp = malloc(n + 4);
    if (!tmp) {
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = b64url[i];
        if (c == '-') {
            tmp[j++] = '+';
        } else if (c == '_') {
            tmp[j++] = '/';
        } else {
            tmp[j++] = c;
        }
    }
    size_t pad = (4 - (j % 4)) % 4;
    for (size_t k = 0; k < pad && j < n + 3; k++) {
        tmp[j++] = '=';
    }
    tmp[j] = '\0';

    size_t olen = 0;
    int ret = mbedtls_base64_decode(dec, dec_size, &olen,
                                   (const unsigned char *)tmp, strlen(tmp));
    free(tmp);

    if (ret != 0) {
        return -1;
    }
    return (int)olen;
}

prov_token_status_t prov_token_check_expiry(const char *jwt, time_t utc_now)
{
    if (jwt == NULL || jwt[0] == '\0') {
        return PROV_TOKEN_STATUS_MALFORMED;
    }

    char enc[1400];
    size_t enc_len = jwt_copy_payload_segment(jwt, enc, sizeof(enc));
    if (enc_len == 0) {
        ESP_LOGW(TAG, "JWT: missing payload segment");
        return PROV_TOKEN_STATUS_MALFORMED;
    }

    unsigned char payload[1024];
    int pay_len = base64url_decode(enc, payload, sizeof(payload) - 1);
    if (pay_len <= 0) {
        ESP_LOGW(TAG, "JWT: payload base64 decode failed");
        return PROV_TOKEN_STATUS_MALFORMED;
    }
    payload[pay_len] = '\0';

    cJSON *root = cJSON_Parse((const char *)payload);
    if (!root) {
        ESP_LOGW(TAG, "JWT: payload is not valid JSON");
        return PROV_TOKEN_STATUS_MALFORMED;
    }

    prov_token_status_t status = PROV_TOKEN_STATUS_MALFORMED;
    time_t deadline = 0;
    bool have_deadline = false;

    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
    if (cJSON_IsNumber(exp)) {
        deadline = (time_t)exp->valuedouble;
        have_deadline = true;
    } else if (cJSON_IsString(exp) && exp->valuestring) {
        deadline = (time_t)strtoll(exp->valuestring, NULL, 10);
        have_deadline = true;
    }

    if (!have_deadline) {
        const cJSON *iat = cJSON_GetObjectItemCaseSensitive(root, "iat");
        time_t iat_val = 0;
        if (cJSON_IsNumber(iat)) {
            iat_val = (time_t)iat->valuedouble;
        } else if (cJSON_IsString(iat) && iat->valuestring) {
            iat_val = (time_t)strtoll(iat->valuestring, NULL, 10);
        }
        if (iat_val > 0) {
            deadline = iat_val + (time_t)PROV_TOKEN_FALLBACK_TTL_SEC;
            have_deadline = true;
            ESP_LOGI(TAG, "JWT: no exp; using iat + %d sec as deadline",
                     (int)PROV_TOKEN_FALLBACK_TTL_SEC);
        }
    }

    if (!have_deadline) {
        ESP_LOGW(TAG, "JWT: no exp or iat in payload");
        cJSON_Delete(root);
        return PROV_TOKEN_STATUS_MALFORMED;
    }

    ESP_LOGI(TAG, "JWT: deadline (exp) = %lld, now = %lld",
             (long long)deadline, (long long)utc_now);

    if (utc_now >= deadline) {
        status = PROV_TOKEN_STATUS_EXPIRED;
    } else {
        status = PROV_TOKEN_STATUS_VALID;
    }

    cJSON_Delete(root);
    return status;
}
