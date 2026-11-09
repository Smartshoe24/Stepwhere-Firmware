/*
 * ota_layout.h - Shared OTA contract for the Stepwhere bootloader, the
 * application and the host packaging tool (tools/ota_tool.py).
 *
 * Everything in this file is a binary format or an address. Once devices are
 * in the field these values must NOT change without bumping
 * OTA_HDR_VERSION / OTA_META_VERSION and handling both versions.
 *
 * See docs/OTA_DESIGN.md for the reasoning behind each value.
 */
#ifndef STEPWHERE_OTA_LAYOUT_H
#define STEPWHERE_OTA_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* STM32G070CBT6 internal flash (128 KB, single bank, 2 KB pages)           */
/* ------------------------------------------------------------------------ */
#define MCU_FLASH_BASE        0x08000000UL
#define MCU_FLASH_SIZE        (128UL * 1024UL)
#define MCU_FLASH_PAGE_SIZE   2048UL

#define MCU_RAM_BASE          0x20000000UL
#define MCU_RAM_SIZE          (36UL * 1024UL)

#define BOOTLOADER_ADDR       MCU_FLASH_BASE
#define BOOTLOADER_SIZE       (16UL * 1024UL)

#define APP_ADDR              (BOOTLOADER_ADDR + BOOTLOADER_SIZE)   /* 0x08004000 */
#define APP_MAX_SIZE          (MCU_FLASH_SIZE - BOOTLOADER_SIZE)    /* 112 KB     */

/* ------------------------------------------------------------------------ */
/* W25Q128 external flash (16 MB, 4 KB sectors, 64 KB blocks)               */
/* ------------------------------------------------------------------------ */
#define EXT_FLASH_SIZE        (16UL * 1024UL * 1024UL)
#define EXT_SECTOR_SIZE       4096UL
#define EXT_BLOCK_SIZE        65536UL

/* Two copies of the boot metadata, one 4 KB sector each. The valid copy with
 * the higher sequence number wins, so a power cut while writing one copy
 * always leaves the other one intact. */
#define EXT_META_A_ADDR       0x000000UL
#define EXT_META_B_ADDR       0x001000UL

/* Image slots: each holds one packaged image (header + payload). 128 KB is
 * larger than APP_MAX_SIZE + header, and keeps every slot 64 KB aligned so
 * it can be erased with fast 64 KB block erases. */
#define EXT_SLOT_SIZE         (128UL * 1024UL)
#define EXT_SLOT_DOWNLOAD     0x010000UL   /* new image being downloaded      */
#define EXT_SLOT_BACKUP       0x030000UL   /* previous app, used for rollback  */
#define EXT_SLOT_FACTORY      0x050000UL   /* known-good image, last resort    */

/* First address free for application data (GPS logs, config, ...). */
#define EXT_APP_DATA_ADDR     0x070000UL

/* Scratch sector used only by the flash self-test. Top of the chip, far away
 * from OTA data. */
#define EXT_SELFTEST_ADDR     (EXT_FLASH_SIZE - EXT_SECTOR_SIZE)    /* 0xFFF000 */

/* ------------------------------------------------------------------------ */
/* Image header (256 bytes, little-endian, placed in front of the payload)  */
/* ------------------------------------------------------------------------ */
#define OTA_HDR_MAGIC         0x57465753UL  /* "SWFW" in memory order */
#define OTA_HDR_VERSION       1u
#define OTA_HDR_SIZE          256u
#define OTA_HDR_SIGNED_LEN    128u          /* bytes [0,128) are signed     */

#define OTA_FLAG_SIGNED       (1UL << 0)    /* signature field is valid     */

/* Hardware identifier: firmware built for one board is refused by another. */
#define OTA_HW_ID_GSM_PCB_R1  0x00010001UL

/* Version packing: v1.2.3 -> 0x00010203 */
#define OTA_VERSION(maj, min, pat) \
    ((((uint32_t)(maj) & 0xFFu) << 16) | (((uint32_t)(min) & 0xFFu) << 8) | ((uint32_t)(pat) & 0xFFu))

typedef struct {
    uint32_t magic;             /*   0 OTA_HDR_MAGIC                        */
    uint16_t hdr_version;       /*   4 OTA_HDR_VERSION                      */
    uint16_t hdr_size;          /*   6 OTA_HDR_SIZE                         */
    uint32_t fw_version;        /*   8 OTA_VERSION(maj, min, patch)         */
    uint32_t image_size;        /*  12 payload size in bytes                */
    uint32_t load_addr;         /*  16 must equal APP_ADDR                  */
    uint32_t hw_id;             /*  20 OTA_HW_ID_*                          */
    uint32_t flags;             /*  24 OTA_FLAG_*                           */
    uint32_t build_time;        /*  28 Unix time of the build               */
    uint8_t  payload_sha256[32];/*  32 SHA-256 of the payload               */
    uint8_t  git_sha1[20];      /*  64 commit the image was built from      */
    uint8_t  reserved0[44];     /*  84 zero                                 */
    /* ---- everything above (128 bytes) is covered by the signature ---- */
    uint8_t  signature[64];     /* 128 ECDSA P-256 r||s over SHA-256(hdr[0,128)) */
    uint8_t  reserved1[60];     /* 192 zero                                 */
    uint32_t hdr_crc32;         /* 252 CRC-32 (IEEE) of bytes [0,252)       */
} ota_image_header_t;

/* ------------------------------------------------------------------------ */
/* Boot metadata (one record at the start of each metadata sector)          */
/* ------------------------------------------------------------------------ */
#define OTA_META_MAGIC        0x4154454DUL  /* "META" */
#define OTA_META_VERSION      1u

typedef enum {
    OTA_STATE_IDLE        = 0,  /* running a confirmed app, nothing to do       */
    OTA_STATE_PENDING     = 1,  /* verified image in download slot, install it  */
    OTA_STATE_BACKING_UP  = 2,  /* copying current app to the backup slot       */
    OTA_STATE_INSTALLING  = 3,  /* programming internal flash from download slot*/
    OTA_STATE_TRIAL       = 4,  /* new app booted, waiting for ota_confirm()    */
    OTA_STATE_ROLLING_BACK= 5   /* restoring the backup slot                    */
} ota_state_t;

#define OTA_MAX_TRIAL_BOOTS   3u

/* ota_boot_meta_t.last_result: outcome of the last update, for reporting. */
typedef enum {
    OTA_RES_NONE           = 0,
    OTA_RES_INSTALLED      = 1,  /* new image installed, now in TRIAL          */
    OTA_RES_CONFIRMED      = 2,  /* new image confirmed by the app             */
    OTA_RES_BAD_IMAGE      = 3,  /* download slot failed verification          */
    OTA_RES_INSTALL_FAILED = 4,  /* internal flash erase/program/verify failed */
    OTA_RES_ROLLED_BACK    = 5,  /* previous image restored                    */
    OTA_RES_NO_BACKUP      = 6   /* rollback needed but no valid backup found  */
} ota_result_t;

typedef struct {
    uint32_t magic;             /* OTA_META_MAGIC                           */
    uint16_t meta_version;      /* OTA_META_VERSION                         */
    uint16_t record_size;       /* sizeof(ota_boot_meta_t)                  */
    uint32_t seq;               /* incremented on every write               */
    uint32_t state;             /* ota_state_t                              */
    uint32_t trial_boots;       /* boots attempted while in OTA_STATE_TRIAL */
    uint32_t installed_version; /* version currently in internal flash      */
    uint32_t installed_size;
    uint8_t  installed_sha256[32];
    uint32_t pending_version;   /* version waiting in the download slot     */
    uint32_t last_result;       /* last update result code, for reporting   */
    uint8_t  reserved[16];
    uint32_t crc32;             /* CRC-32 (IEEE) of all bytes before it     */
} ota_boot_meta_t;

/* Compile-time checks that the structs match the documented sizes. */
#if defined(__cplusplus)
static_assert(sizeof(ota_image_header_t) == OTA_HDR_SIZE, "ota_image_header_t must be 256 bytes");
static_assert(sizeof(ota_boot_meta_t) == 88, "ota_boot_meta_t must be 88 bytes");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ota_image_header_t) == OTA_HDR_SIZE, "ota_image_header_t must be 256 bytes");
_Static_assert(sizeof(ota_boot_meta_t) == 88, "ota_boot_meta_t must be 88 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_OTA_LAYOUT_H */
