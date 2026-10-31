/*
 * w25q.h - Minimal driver for Winbond W25Q128FV / W25Q128JV SPI NOR flash.
 *
 * Plain C with no Arduino or HAL dependency, so the same code runs in the
 * application (Arduino SPI, see W25QArduino.h) and in the bootloader (LL SPI).
 * The caller provides the SPI transport as a few function pointers.
 *
 * Only standard single-bit SPI commands are used (datasheet table 8.1.2).
 */
#ifndef STEPWHERE_W25Q_H
#define STEPWHERE_W25Q_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define W25Q_PAGE_SIZE      256u
#define W25Q_SECTOR_SIZE    4096u
#define W25Q_BLOCK_SIZE     65536u
#define W25Q_CAPACITY       (16UL * 1024UL * 1024UL)

#define W25Q_MANUF_WINBOND  0xEFu
#define W25Q_ID_128_IQ      0x4018u   /* W25Q128FV and W25Q128JV-IQ/JQ */
#define W25Q_ID_128_IM      0x7018u   /* W25Q128JV-IM/JM               */

typedef enum {
    W25Q_OK = 0,
    W25Q_ERR_PARAM,      /* bad address, length or alignment            */
    W25Q_ERR_ID,         /* JEDEC ID is not a supported W25Q128         */
    W25Q_ERR_TIMEOUT,    /* BUSY did not clear in time                  */
    W25Q_ERR_WEL,        /* write enable latch did not set              */
    W25Q_ERR_VERIFY      /* read-back after program did not match       */
} w25q_status_t;

/* SPI transport supplied by the caller. ctx is passed back unchanged. */
typedef struct {
    void *ctx;
    /* Drive /CS: active=true pulls /CS low. */
    void (*select)(void *ctx, bool active);
    /* Full-duplex transfer of len bytes. tx may be NULL (send 0xFF),
     * rx may be NULL (discard). */
    void (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    /* Milliseconds since boot, used for timeouts. */
    uint32_t (*millis)(void *ctx);
    /* Busy-wait for at least us microseconds. */
    void (*delay_us)(void *ctx, uint32_t us);
} w25q_bus_t;

typedef struct {
    w25q_bus_t bus;
    uint8_t    manufacturer;
    uint16_t   device_id;
    bool       powered_down;
} w25q_t;

/* Wake the chip, software-reset it and check the JEDEC ID.
 * Call at least 5 ms after power-up (tPUW) before erasing or writing. */
w25q_status_t w25q_init(w25q_t *dev, const w25q_bus_t *bus);

w25q_status_t w25q_read_jedec_id(w25q_t *dev, uint8_t *manufacturer, uint16_t *device_id);
w25q_status_t w25q_read_unique_id(w25q_t *dev, uint8_t uid[8]);

/* Read any length from any address (wraps are rejected). */
w25q_status_t w25q_read(w25q_t *dev, uint32_t addr, void *buf, size_t len);

/* Program any length at any address. Splits at 256-byte page boundaries.
 * The target range must already be erased (0xFF); flash can only clear bits. */
w25q_status_t w25q_write(w25q_t *dev, uint32_t addr, const void *buf, size_t len);

/* Like w25q_write, then reads the data back and compares it. */
w25q_status_t w25q_write_verify(w25q_t *dev, uint32_t addr, const void *buf, size_t len);

/* Erase one 4 KB sector / one 64 KB block. addr must be aligned. */
w25q_status_t w25q_erase_sector(w25q_t *dev, uint32_t addr);
w25q_status_t w25q_erase_block64(w25q_t *dev, uint32_t addr);

/* Erase [addr, addr+len). Both must be 4 KB aligned. Uses 64 KB block erase
 * wherever possible and 4 KB sector erase for the edges. */
w25q_status_t w25q_erase_range(w25q_t *dev, uint32_t addr, uint32_t len);

/* Deep power-down (about 1 uA instead of 10 uA standby). Any other driver
 * call wakes the chip automatically. */
w25q_status_t w25q_power_down(w25q_t *dev);
w25q_status_t w25q_wake(w25q_t *dev);

const char *w25q_status_str(w25q_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_W25Q_H */
