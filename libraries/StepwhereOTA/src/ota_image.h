/*
 * ota_image.h - Checks for packaged OTA images (header + payload) stored in
 * the W25Q128. Used by the bootloader before installing and by the app
 * after a download.
 *
 * Signature verification is not implemented yet: images are accepted on
 * header CRC + SHA-256 of the payload. See docs/OTA_DESIGN.md.
 */
#ifndef STEPWHERE_OTA_IMAGE_H
#define STEPWHERE_OTA_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_layout.h"
#include "w25q.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_IMG_OK = 0,
    OTA_IMG_ERR_FLASH,      /* external flash read failed                */
    OTA_IMG_ERR_MAGIC,      /* no image (erased slot) or not ours        */
    OTA_IMG_ERR_HDR_CRC,    /* header damaged                            */
    OTA_IMG_ERR_HDR,        /* unsupported header version/size, load addr*/
    OTA_IMG_ERR_HW_ID,      /* built for another board                   */
    OTA_IMG_ERR_SIZE,       /* payload empty or larger than app area     */
    OTA_IMG_ERR_VECTORS,    /* payload is not an app linked at APP_ADDR  */
    OTA_IMG_ERR_HASH        /* payload SHA-256 mismatch                  */
} ota_img_status_t;

/* True if (sp, reset) look like the first two vector table words of an
 * application linked at APP_ADDR. */
bool ota_app_vectors_ok(uint32_t sp, uint32_t reset);

/* Header-only checks (magic, CRC, version, load address, hw id, size). */
ota_img_status_t ota_image_check_header(const ota_image_header_t *hdr);

/* Full check of the image stored at slot_addr: header checks, vector table,
 * and SHA-256 over the whole payload. buf is scratch space (>= 64 bytes;
 * larger is faster). hdr_out may be NULL. */
ota_img_status_t ota_image_check_slot(w25q_t *dev, uint32_t slot_addr,
                                      ota_image_header_t *hdr_out,
                                      uint8_t *buf, size_t buf_len);

/* Fill in hdr->hdr_crc32 from the other fields. */
void ota_image_header_seal(ota_image_header_t *hdr);

const char *ota_img_status_str(ota_img_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_OTA_IMAGE_H */
