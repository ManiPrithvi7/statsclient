/* JWT provisioning token expiry validation (no signature verification).
 *
 * Server tokens are short-lived; we decode the payload and compare `exp` to UTC.
 */

#ifndef PROV_TOKEN_VALIDATE_H
#define PROV_TOKEN_VALIDATE_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** If JWT has no `exp`, treat as expired when now >= iat + this (seconds). */
#ifndef PROV_TOKEN_FALLBACK_TTL_SEC
#define PROV_TOKEN_FALLBACK_TTL_SEC (600)
#endif

typedef enum {
    PROV_TOKEN_STATUS_VALID = 0,
    PROV_TOKEN_STATUS_EXPIRED,
    PROV_TOKEN_STATUS_MALFORMED,
} prov_token_status_t;

/**
 * @brief Check whether a JWT-shaped provisioning token is expired at utc_now.
 *
 * Uses `exp` if present. If `exp` is missing but `iat` is present, uses
 * iat + PROV_TOKEN_FALLBACK_TTL_SEC. If neither is usable, returns MALFORMED.
 *
 * @param jwt       Null-terminated JWT string
 * @param utc_now   Current UTC time from time() after SNTP sync
 */
prov_token_status_t prov_token_check_expiry(const char *jwt, time_t utc_now);

#ifdef __cplusplus
}
#endif

#endif /* PROV_TOKEN_VALIDATE_H */
