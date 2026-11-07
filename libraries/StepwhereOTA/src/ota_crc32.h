/*
 * ota_crc32.h - CRC-32 (IEEE 802.3, same as zlib/binascii.crc32).
 * Small bitwise implementation: only used on headers and metadata.
 */
#ifndef STEPWHERE_OTA_CRC32_H
#define STEPWHERE_OTA_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start with crc = 0; feed the result back in to continue. */
uint32_t ota_crc32_update(uint32_t crc, const void *data, size_t len);

static inline uint32_t ota_crc32(const void *data, size_t len)
{
    return ota_crc32_update(0, data, len);
}

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_OTA_CRC32_H */
