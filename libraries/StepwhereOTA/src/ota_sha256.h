/*
 * ota_sha256.h - Compact SHA-256 (FIPS 180-4) for image verification.
 */
#ifndef STEPWHERE_OTA_SHA256_H
#define STEPWHERE_OTA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SHA256_LEN 32u

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t  block[64];
    uint32_t block_len;
} ota_sha256_t;

void ota_sha256_init(ota_sha256_t *ctx);
void ota_sha256_update(ota_sha256_t *ctx, const void *data, size_t len);
void ota_sha256_final(ota_sha256_t *ctx, uint8_t out[OTA_SHA256_LEN]);
void ota_sha256(const void *data, size_t len, uint8_t out[OTA_SHA256_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_OTA_SHA256_H */
