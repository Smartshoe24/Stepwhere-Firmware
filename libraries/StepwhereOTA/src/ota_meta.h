/*
 * ota_meta.h - Boot metadata stored twice in the W25Q128 (sectors A and B).
 *
 * Each save goes to the sector that does NOT hold the newest valid record,
 * with a higher sequence number. A power cut during a save therefore only
 * damages the older copy, and the newest valid record is always kept.
 */
#ifndef STEPWHERE_OTA_META_H
#define STEPWHERE_OTA_META_H

#include <stdbool.h>

#include "ota_layout.h"
#include "w25q.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the newest valid record. Returns false if neither copy is valid; *m
 * is then set to a default record (state IDLE, everything zero). */
bool ota_meta_load(w25q_t *dev, ota_boot_meta_t *m);

/* Save *m as the new newest record (seq, magic and CRC are filled in). */
w25q_status_t ota_meta_save(w25q_t *dev, ota_boot_meta_t *m);

/* App side: call after the new firmware has passed its self-test.
 * TRIAL -> IDLE. Does nothing (and writes nothing) in other states. */
w25q_status_t ota_meta_confirm(w25q_t *dev);

/* App side: call after a download has been verified with
 * ota_image_check_slot(EXT_SLOT_DOWNLOAD). The bootloader installs it on the
 * next reset. */
w25q_status_t ota_meta_set_pending(w25q_t *dev, uint32_t version);

const char *ota_state_str(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_OTA_META_H */
