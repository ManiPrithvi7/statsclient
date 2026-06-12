#ifndef OTA_MANIFEST_H
#define OTA_MANIFEST_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[32];
    char download_url[512];
    char sha256[65];
    char signature[256];
    uint32_t size_bytes;
} ota_manifest_t;

#ifdef __cplusplus
}
#endif

#endif /* OTA_MANIFEST_H */
