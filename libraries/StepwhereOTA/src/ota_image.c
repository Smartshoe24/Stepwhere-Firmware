#include "ota_image.h"

#include <string.h>

#include "ota_crc32.h"
#include "ota_sha256.h"

#define HDR_CRC_LEN (OTA_HDR_SIZE - 4u)   /* CRC covers bytes [0, 252) */

bool ota_app_vectors_ok(uint32_t sp, uint32_t reset)
{
    uint32_t pc = reset & ~1UL;
    return sp > MCU_RAM_BASE && sp <= MCU_RAM_BASE + MCU_RAM_SIZE && (sp & 3u) == 0u &&
           (reset & 1u) == 1u && pc >= APP_ADDR + 8u && pc < APP_ADDR + APP_MAX_SIZE;
}

void ota_image_header_seal(ota_image_header_t *hdr)
{
    hdr->hdr_crc32 = ota_crc32(hdr, HDR_CRC_LEN);
}

ota_img_status_t ota_image_check_header(const ota_image_header_t *hdr)
{
    if (hdr->magic != OTA_HDR_MAGIC) {
        return OTA_IMG_ERR_MAGIC;
    }
    if (ota_crc32(hdr, HDR_CRC_LEN) != hdr->hdr_crc32) {
        return OTA_IMG_ERR_HDR_CRC;
    }
    if (hdr->hdr_version != OTA_HDR_VERSION || hdr->hdr_size != OTA_HDR_SIZE ||
        hdr->load_addr != APP_ADDR) {
        return OTA_IMG_ERR_HDR;
    }
    if (hdr->hw_id != OTA_HW_ID_GSM_PCB_R1) {
        return OTA_IMG_ERR_HW_ID;
    }
    if (hdr->image_size < 8u || hdr->image_size > APP_MAX_SIZE) {
        return OTA_IMG_ERR_SIZE;
    }
    return OTA_IMG_OK;
}

ota_img_status_t ota_image_check_slot(w25q_t *dev, uint32_t slot_addr,
                                      ota_image_header_t *hdr_out,
                                      uint8_t *buf, size_t buf_len)
{
    ota_image_header_t hdr;
    ota_sha256_t sha;
    uint8_t digest[OTA_SHA256_LEN];
    ota_img_status_t st;
    uint32_t addr, left;

    if (buf_len < 64u) {
        return OTA_IMG_ERR_FLASH;
    }
    if (w25q_read(dev, slot_addr, &hdr, sizeof hdr) != W25Q_OK) {
        return OTA_IMG_ERR_FLASH;
    }
    st = ota_image_check_header(&hdr);
    if (st != OTA_IMG_OK) {
        return st;
    }

    ota_sha256_init(&sha);
    addr = slot_addr + OTA_HDR_SIZE;
    left = hdr.image_size;
    while (left > 0u) {
        size_t n = left < buf_len ? left : buf_len;
        if (w25q_read(dev, addr, buf, n) != W25Q_OK) {
            return OTA_IMG_ERR_FLASH;
        }
        if (addr == slot_addr + OTA_HDR_SIZE) {
            uint32_t sp, reset;
            memcpy(&sp, buf, 4);
            memcpy(&reset, buf + 4, 4);
            if (!ota_app_vectors_ok(sp, reset)) {
                return OTA_IMG_ERR_VECTORS;
            }
        }
        ota_sha256_update(&sha, buf, n);
        addr += (uint32_t)n;
        left -= (uint32_t)n;
    }
    ota_sha256_final(&sha, digest);
    if (memcmp(digest, hdr.payload_sha256, sizeof digest) != 0) {
        return OTA_IMG_ERR_HASH;
    }
    if (hdr_out) {
        *hdr_out = hdr;
    }
    return OTA_IMG_OK;
}

const char *ota_img_status_str(ota_img_status_t st)
{
    switch (st) {
    case OTA_IMG_OK:          return "ok";
    case OTA_IMG_ERR_FLASH:   return "flash read error";
    case OTA_IMG_ERR_MAGIC:   return "no image";
    case OTA_IMG_ERR_HDR_CRC: return "header CRC error";
    case OTA_IMG_ERR_HDR:     return "unsupported header";
    case OTA_IMG_ERR_HW_ID:   return "wrong hardware id";
    case OTA_IMG_ERR_SIZE:    return "bad size";
    case OTA_IMG_ERR_VECTORS: return "not linked at 0x08004000";
    case OTA_IMG_ERR_HASH:    return "SHA-256 mismatch";
    default:                  return "unknown";
    }
}
