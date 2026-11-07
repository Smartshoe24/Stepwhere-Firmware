#include "ota_crc32.h"

uint32_t ota_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFUL;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1UL)));
        }
    }
    return c ^ 0xFFFFFFFFUL;
}
