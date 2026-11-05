/*
 * motion_log.h - Raw accelerometer log in the W25Q128 external flash.
 *
 * Lets the board record while it is inside the shoe with no cable attached;
 * the data is read out over the debug UART afterwards.
 *
 * Area: EXT_APP_DATA_ADDR (0x070000) up to 0xFF0000 (15.5 MB, outside every
 * OTA region and below the flash self-test block). At 50 Hz that is about
 * 13 hours of data.
 *
 * The area is used like a tape: 256-byte pages written one after the other,
 * everything after the write position is erased (0xFF). Every power-up or
 * reset starts a new session number. Page layout (little-endian):
 *
 *   0  u16 magic 0x4D53 ("SM")      8  u16 sample rate (Hz)
 *   2  u8  session (1..254)        10  u16 flags (0)
 *   3  u8  samples in page (1..40) 12  int16 x,y,z mg x 40 samples (240 B)
 *   4  u32 time of first sample,  252  u16 CRC-16/CCITT of bytes 0..251
 *          ms since session start 254  u16 0
 *
 * Samples inside a page are 1/rate apart. A jump in the page time means
 * samples were lost (FIFO overrun).
 */
#ifndef STEPWHERE_MOTION_LOG_H
#define STEPWHERE_MOTION_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "w25q.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MLOG_START          0x070000UL   /* = EXT_APP_DATA_ADDR (ota_layout.h) */
#define MLOG_END            0xFF0000UL   /* below the self-test block           */
#define MLOG_PAGE_SIZE      256u
#define MLOG_SAMPLES        40u
#define MLOG_MAGIC          0x4D53u

typedef enum {
    MLOG_OK = 0,
    MLOG_ERR_FLASH,       /* SPI flash error                                 */
    MLOG_ERR_FULL,        /* log area full: download, then erase             */
    MLOG_ERR_NOT_ERASED,  /* next page is not blank: erase the log           */
    MLOG_ERR_PARAM
} mlog_status_t;

typedef enum {
    MLOG_PAGE_VALID = 0,
    MLOG_PAGE_ERASED,
    MLOG_PAGE_CORRUPT
} mlog_page_kind_t;

typedef struct {
    uint8_t  session;
    uint8_t  n;
    uint32_t t0_ms;
    uint16_t hz;
    int16_t  s[MLOG_SAMPLES][3];
} mlog_page_t;

typedef struct {
    w25q_t  *flash;
    uint32_t start, end;     /* byte addresses, page aligned                   */
    uint32_t wr;             /* next page to write                             */
    uint8_t  last_session;   /* session number found in the log (0 = none)     */
    uint8_t  session;        /* current session, 0 = not recording             */
    uint16_t hz;
    uint32_t sample_ms_num;  /* 1000, divided by hz for sample times           */
    uint32_t next_t_ms;      /* time of the next sample in the session         */
    mlog_page_t buf;
} mlog_t;

/* Find the write position (binary search over the pages) and the last
 * session number. start/end 0 = default area. */
mlog_status_t mlog_open(mlog_t *log, w25q_t *flash, uint32_t start, uint32_t end);

/* Start a new session (number last_session + 1). */
mlog_status_t mlog_begin(mlog_t *log, uint16_t hz);

/* Add one sample. t_ms is the session time of this sample; pass the value
 * from mlog_next_time() unless samples were lost. Writes a page when 40
 * samples are collected. */
mlog_status_t mlog_add(mlog_t *log, uint32_t t_ms, int16_t x, int16_t y, int16_t z);

/* Session time the next sample would have without gaps. */
static inline uint32_t mlog_next_time(const mlog_t *log) { return log->next_t_ms; }

/* Write the partly filled page (call before stopping). */
mlog_status_t mlog_flush(mlog_t *log);

/* Flush and end the session. */
mlog_status_t mlog_end(mlog_t *log);

/* Read page number idx (0 = first page of the area). */
mlog_page_kind_t mlog_read_page(mlog_t *log, uint32_t idx, mlog_page_t *out);

/* Pages written / capacity. */
static inline uint32_t mlog_pages_used(const mlog_t *log) { return (log->wr - log->start) / MLOG_PAGE_SIZE; }
static inline uint32_t mlog_pages_total(const mlog_t *log) { return (log->end - log->start) / MLOG_PAGE_SIZE; }

/* Erase the used part of the area (64 KB blocks) so recording starts again
 * at the beginning. full = erase the whole area (first use, or after
 * MLOG_ERR_NOT_ERASED). progress(ctx, done, total) is called per block. */
mlog_status_t mlog_erase(mlog_t *log, bool full, void (*progress)(void *ctx, uint32_t done, uint32_t total),
                         void *ctx);

uint16_t mlog_crc16(const uint8_t *p, uint32_t n);
const char *mlog_status_str(mlog_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_MOTION_LOG_H */
