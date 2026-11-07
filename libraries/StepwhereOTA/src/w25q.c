/*
 * w25q.c - Minimal driver for Winbond W25Q128FV / W25Q128JV SPI NOR flash.
 * Command codes and timings: W25Q128JV datasheet rev F, sections 8 and 9.6.
 */
#include "w25q.h"

#include <string.h>

/* Standard SPI instructions (datasheet 8.1.2) */
#define CMD_WRITE_ENABLE     0x06u
#define CMD_READ_SR1         0x05u
#define CMD_READ_DATA        0x03u   /* max 50 MHz */
#define CMD_PAGE_PROGRAM     0x02u
#define CMD_SECTOR_ERASE     0x20u   /* 4 KB  */
#define CMD_BLOCK_ERASE_64K  0xD8u   /* 64 KB */
#define CMD_POWER_DOWN       0xB9u
#define CMD_RELEASE_PD       0xABu
#define CMD_JEDEC_ID         0x9Fu
#define CMD_UNIQUE_ID        0x4Bu
#define CMD_ENABLE_RESET     0x66u
#define CMD_RESET            0x99u

#define SR1_BUSY             0x01u
#define SR1_WEL              0x02u

/* Max times from datasheet 9.6, with margin. */
#define TIMEOUT_PROGRAM_MS   10u     /* tPP max 3 ms      */
#define TIMEOUT_SECTOR_MS    1000u   /* tSE max 400 ms    */
#define TIMEOUT_BLOCK_MS     3000u   /* tBE2 max 2000 ms  */
#define DELAY_RELEASE_PD_US  5u      /* tRES1 max 3 us    */
#define DELAY_RESET_US       50u     /* tRST max 30 us    */

/* Chunk size used when comparing read-back data in w25q_write_verify. */
#define VERIFY_CHUNK         64u

/* ------------------------------------------------------------------------ */

static void cmd_only(w25q_t *dev, uint8_t cmd)
{
    dev->bus.select(dev->bus.ctx, true);
    dev->bus.transfer(dev->bus.ctx, &cmd, NULL, 1);
    dev->bus.select(dev->bus.ctx, false);
}

static void cmd_addr(uint8_t out[4], uint8_t cmd, uint32_t addr)
{
    out[0] = cmd;
    out[1] = (uint8_t)(addr >> 16);
    out[2] = (uint8_t)(addr >> 8);
    out[3] = (uint8_t)addr;
}

static uint8_t read_sr1(w25q_t *dev)
{
    uint8_t cmd = CMD_READ_SR1;
    uint8_t sr = 0xFF;
    dev->bus.select(dev->bus.ctx, true);
    dev->bus.transfer(dev->bus.ctx, &cmd, NULL, 1);
    dev->bus.transfer(dev->bus.ctx, NULL, &sr, 1);
    dev->bus.select(dev->bus.ctx, false);
    return sr;
}

static w25q_status_t wait_ready(w25q_t *dev, uint32_t timeout_ms)
{
    uint32_t start = dev->bus.millis(dev->bus.ctx);
    for (;;) {
        if ((read_sr1(dev) & SR1_BUSY) == 0u) {
            return W25Q_OK;
        }
        /* Unsigned subtraction handles millis() wrap-around. */
        if ((uint32_t)(dev->bus.millis(dev->bus.ctx) - start) > timeout_ms) {
            return W25Q_ERR_TIMEOUT;
        }
    }
}

static w25q_status_t write_enable(w25q_t *dev)
{
    cmd_only(dev, CMD_WRITE_ENABLE);
    return (read_sr1(dev) & SR1_WEL) ? W25Q_OK : W25Q_ERR_WEL;
}

static bool range_ok(uint32_t addr, size_t len)
{
    return addr < W25Q_CAPACITY && len <= (size_t)(W25Q_CAPACITY - addr);
}

/* ------------------------------------------------------------------------ */

w25q_status_t w25q_wake(w25q_t *dev)
{
    if (dev->powered_down) {
        cmd_only(dev, CMD_RELEASE_PD);
        dev->bus.delay_us(dev->bus.ctx, DELAY_RELEASE_PD_US);
        dev->powered_down = false;
    }
    return W25Q_OK;
}

w25q_status_t w25q_power_down(w25q_t *dev)
{
    w25q_status_t st = wait_ready(dev, TIMEOUT_BLOCK_MS);
    if (st != W25Q_OK) {
        return st;
    }
    cmd_only(dev, CMD_POWER_DOWN);
    dev->bus.delay_us(dev->bus.ctx, DELAY_RELEASE_PD_US);   /* tDP max 3 us */
    dev->powered_down = true;
    return W25Q_OK;
}

w25q_status_t w25q_read_jedec_id(w25q_t *dev, uint8_t *manufacturer, uint16_t *device_id)
{
    uint8_t cmd = CMD_JEDEC_ID;
    uint8_t id[3];
    w25q_wake(dev);
    dev->bus.select(dev->bus.ctx, true);
    dev->bus.transfer(dev->bus.ctx, &cmd, NULL, 1);
    dev->bus.transfer(dev->bus.ctx, NULL, id, sizeof id);
    dev->bus.select(dev->bus.ctx, false);
    if (manufacturer) *manufacturer = id[0];
    if (device_id)    *device_id = (uint16_t)(((uint16_t)id[1] << 8) | id[2]);
    return W25Q_OK;
}

w25q_status_t w25q_read_unique_id(w25q_t *dev, uint8_t uid[8])
{
    uint8_t hdr[5] = { CMD_UNIQUE_ID, 0, 0, 0, 0 };  /* 4 dummy bytes */
    w25q_wake(dev);
    dev->bus.select(dev->bus.ctx, true);
    dev->bus.transfer(dev->bus.ctx, hdr, NULL, sizeof hdr);
    dev->bus.transfer(dev->bus.ctx, NULL, uid, 8);
    dev->bus.select(dev->bus.ctx, false);
    return W25Q_OK;
}

w25q_status_t w25q_init(w25q_t *dev, const w25q_bus_t *bus)
{
    w25q_status_t st;

    if (!dev || !bus || !bus->select || !bus->transfer || !bus->millis || !bus->delay_us) {
        return W25Q_ERR_PARAM;
    }
    memset(dev, 0, sizeof *dev);
    dev->bus = *bus;
    dev->bus.select(dev->bus.ctx, false);

    /* The chip may still be in deep power-down from before an MCU reset.
     * Release it unconditionally; the command is harmless otherwise. */
    dev->powered_down = true;
    w25q_wake(dev);

    /* If the MCU was reset in the middle of an erase, let the chip finish it
     * before the software reset, which would otherwise abort it and leave
     * the sector half-erased. */
    st = wait_ready(dev, TIMEOUT_BLOCK_MS);
    if (st != W25Q_OK) {
        return st;
    }
    cmd_only(dev, CMD_ENABLE_RESET);
    cmd_only(dev, CMD_RESET);
    dev->bus.delay_us(dev->bus.ctx, DELAY_RESET_US);

    w25q_read_jedec_id(dev, &dev->manufacturer, &dev->device_id);
    if (dev->manufacturer != W25Q_MANUF_WINBOND ||
        (dev->device_id != W25Q_ID_128_IQ && dev->device_id != W25Q_ID_128_IM)) {
        return W25Q_ERR_ID;
    }
    return W25Q_OK;
}

w25q_status_t w25q_read(w25q_t *dev, uint32_t addr, void *buf, size_t len)
{
    uint8_t hdr[4];
    w25q_status_t st;

    if (!buf || !range_ok(addr, len)) {
        return W25Q_ERR_PARAM;
    }
    if (len == 0) {
        return W25Q_OK;
    }
    w25q_wake(dev);
    st = wait_ready(dev, TIMEOUT_BLOCK_MS);
    if (st != W25Q_OK) {
        return st;
    }
    cmd_addr(hdr, CMD_READ_DATA, addr);
    dev->bus.select(dev->bus.ctx, true);
    dev->bus.transfer(dev->bus.ctx, hdr, NULL, sizeof hdr);
    dev->bus.transfer(dev->bus.ctx, NULL, (uint8_t *)buf, len);
    dev->bus.select(dev->bus.ctx, false);
    return W25Q_OK;
}

w25q_status_t w25q_write(w25q_t *dev, uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t hdr[4];
    w25q_status_t st;

    if (!buf || !range_ok(addr, len)) {
        return W25Q_ERR_PARAM;
    }
    w25q_wake(dev);
    while (len > 0) {
        /* Never cross a 256-byte page: the chip would wrap inside the page. */
        size_t room = W25Q_PAGE_SIZE - (addr % W25Q_PAGE_SIZE);
        size_t n = len < room ? len : room;

        st = wait_ready(dev, TIMEOUT_BLOCK_MS);
        if (st != W25Q_OK) return st;
        st = write_enable(dev);
        if (st != W25Q_OK) return st;

        cmd_addr(hdr, CMD_PAGE_PROGRAM, addr);
        dev->bus.select(dev->bus.ctx, true);
        dev->bus.transfer(dev->bus.ctx, hdr, NULL, sizeof hdr);
        dev->bus.transfer(dev->bus.ctx, src, NULL, n);
        dev->bus.select(dev->bus.ctx, false);

        st = wait_ready(dev, TIMEOUT_PROGRAM_MS);
        if (st != W25Q_OK) return st;

        addr += (uint32_t)n;
        src += n;
        len -= n;
    }
    return W25Q_OK;
}

w25q_status_t w25q_write_verify(w25q_t *dev, uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t chunk[VERIFY_CHUNK];
    w25q_status_t st = w25q_write(dev, addr, buf, len);
    if (st != W25Q_OK) {
        return st;
    }
    while (len > 0) {
        size_t n = len < sizeof chunk ? len : sizeof chunk;
        st = w25q_read(dev, addr, chunk, n);
        if (st != W25Q_OK) return st;
        if (memcmp(chunk, src, n) != 0) return W25Q_ERR_VERIFY;
        addr += (uint32_t)n;
        src += n;
        len -= n;
    }
    return W25Q_OK;
}

static w25q_status_t erase_cmd(w25q_t *dev, uint8_t cmd, uint32_t addr, uint32_t timeout_ms)
{
    uint8_t hdr[4];
    w25q_status_t st;

    w25q_wake(dev);
    st = wait_ready(dev, TIMEOUT_BLOCK_MS);
    if (st != W25Q_OK) return st;
    st = write_enable(dev);
    if (st != W25Q_OK) return st;

    cmd_addr(hdr, cmd, addr);
    dev->bus.select(dev->bus.ctx, true);
    dev->bus.transfer(dev->bus.ctx, hdr, NULL, sizeof hdr);
    dev->bus.select(dev->bus.ctx, false);
    return wait_ready(dev, timeout_ms);
}

w25q_status_t w25q_erase_sector(w25q_t *dev, uint32_t addr)
{
    if (addr >= W25Q_CAPACITY || (addr % W25Q_SECTOR_SIZE) != 0u) {
        return W25Q_ERR_PARAM;
    }
    return erase_cmd(dev, CMD_SECTOR_ERASE, addr, TIMEOUT_SECTOR_MS);
}

w25q_status_t w25q_erase_block64(w25q_t *dev, uint32_t addr)
{
    if (addr >= W25Q_CAPACITY || (addr % W25Q_BLOCK_SIZE) != 0u) {
        return W25Q_ERR_PARAM;
    }
    return erase_cmd(dev, CMD_BLOCK_ERASE_64K, addr, TIMEOUT_BLOCK_MS);
}

w25q_status_t w25q_erase_range(w25q_t *dev, uint32_t addr, uint32_t len)
{
    w25q_status_t st;
    uint32_t end;

    if ((addr % W25Q_SECTOR_SIZE) != 0u || (len % W25Q_SECTOR_SIZE) != 0u ||
        !range_ok(addr, len)) {
        return W25Q_ERR_PARAM;
    }
    end = addr + len;
    while (addr < end) {
        if ((addr % W25Q_BLOCK_SIZE) == 0u && (end - addr) >= W25Q_BLOCK_SIZE) {
            st = w25q_erase_block64(dev, addr);
            addr += W25Q_BLOCK_SIZE;
        } else {
            st = w25q_erase_sector(dev, addr);
            addr += W25Q_SECTOR_SIZE;
        }
        if (st != W25Q_OK) {
            return st;
        }
    }
    return W25Q_OK;
}

const char *w25q_status_str(w25q_status_t st)
{
    switch (st) {
    case W25Q_OK:          return "OK";
    case W25Q_ERR_PARAM:   return "bad parameter";
    case W25Q_ERR_ID:      return "unexpected JEDEC ID";
    case W25Q_ERR_TIMEOUT: return "timeout waiting for BUSY";
    case W25Q_ERR_WEL:     return "write enable failed";
    case W25Q_ERR_VERIFY:  return "read-back mismatch";
    default:               return "unknown";
    }
}
