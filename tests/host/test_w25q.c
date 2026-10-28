/*
 * test_w25q.c - Host unit tests for the w25q driver.
 *
 * Runs the real driver code against a simulated W25Q128 that follows the
 * datasheet rules the driver relies on:
 *   - program/erase need WEL, and WEL clears after each one
 *   - only Read Status (05h) is accepted while BUSY
 *   - page program wraps inside its 256-byte page
 *   - programming can only clear bits (AND with old contents)
 *   - in deep power-down only Release (ABh) is accepted
 * Any rule the driver breaks is counted as a "violation".
 *
 * Build and run:  tests/host/run_tests.sh
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_layout.h"
#include "w25q.h"

/* ---------------------------- simulated chip ---------------------------- */

typedef struct {
    uint8_t *mem;
    uint16_t jedec_dev;        /* device ID to report                     */
    int      busy_polls;       /* SR reads left before BUSY clears        */
    int      stuck_busy;       /* never leave BUSY                        */
    int      write_protected;  /* 06h does not set WEL                    */
    int      wel, pd, reset_en;
    /* current /CS frame */
    int      selected;
    size_t   pos;
    uint8_t  cmd;
    uint32_t addr;
    uint8_t  page[256];
    size_t   page_len;
    /* stats */
    int      violations;
    int      n_sector_erase, n_block_erase, n_program, n_reset;
} fake_t;

static fake_t F;
static uint32_t fake_ms;

static void fake_reset_state(void)
{
    F.wel = 0;
    F.reset_en = 0;
}

static uint8_t fake_byte(uint8_t in)
{
    size_t p = F.pos++;
    if (p == 0) {
        F.cmd = in;
        F.addr = 0;
        F.page_len = 0;
        if (F.pd && in != 0xAB) {
            F.violations++;          /* command sent while powered down */
        }
        /* Command sent while busy. ABh is exempt: init sends it blindly in
         * case the chip is powered down, and a busy chip just ignores it. */
        if ((F.busy_polls > 0 || F.stuck_busy) && in != 0x05 && in != 0xAB) {
            F.violations++;
        }
        return 0xFF;
    }
    if (F.pd) {
        return 0xFF;                 /* DO is high-Z in power-down */
    }
    switch (F.cmd) {
    case 0x05: {
        uint8_t sr = (uint8_t)(F.wel ? 0x02 : 0x00);
        if (F.stuck_busy) {
            sr |= 0x01;
        } else if (F.busy_polls > 0) {
            sr |= 0x01;
            F.busy_polls--;
        }
        return sr;
    }
    case 0x9F: {
        const uint8_t id[3] = { 0xEF, (uint8_t)(F.jedec_dev >> 8), (uint8_t)F.jedec_dev };
        return p <= 3 ? id[p - 1] : 0xFF;
    }
    case 0x4B:
        return p >= 5 ? (uint8_t)(0x10 + (p - 5)) : 0xFF;
    case 0x03:
    case 0x02:
    case 0x20:
    case 0xD8:
        if (p <= 3) {
            F.addr = (F.addr << 8) | in;
            return 0xFF;
        }
        if (F.cmd == 0x03) {
            return F.mem[(F.addr + (uint32_t)(p - 4)) & 0xFFFFFFu];
        }
        if (F.cmd == 0x02) {
            if (F.page_len < sizeof F.page) {
                F.page[F.page_len] = in;
            }
            F.page_len++;
        }
        return 0xFF;
    default:
        return 0xFF;
    }
}

static void fake_end_frame(void)
{
    uint8_t cmd = F.cmd;
    int was_busy = F.busy_polls > 0 || F.stuck_busy;

    if (F.pos == 0) return;
    if (F.pd) {
        if (cmd == 0xAB) F.pd = 0;
        return;
    }
    if (was_busy && cmd != 0x05) {
        return;                      /* ignored by the chip */
    }
    if (cmd != 0x99) {
        F.reset_en = (cmd == 0x66);
    }
    switch (cmd) {
    case 0x06:
        if (!F.write_protected) F.wel = 1;
        break;
    case 0x02:
        if (!F.wel) { F.violations++; break; }
        if (F.page_len > 256) { F.violations++; }   /* driver must not overflow a page */
        for (size_t i = 0; i < F.page_len && i < 256; i++) {
            uint32_t a = (F.addr & 0xFFFF00u) | ((F.addr + (uint32_t)i) & 0xFFu);
            F.mem[a] &= F.page[i];
        }
        F.wel = 0;
        F.busy_polls = 2;
        F.n_program++;
        break;
    case 0x20:
    case 0xD8: {
        uint32_t size = cmd == 0x20 ? 4096u : 65536u;
        if (!F.wel) { F.violations++; break; }
        memset(F.mem + (F.addr & ~(size - 1u)), 0xFF, size);
        F.wel = 0;
        F.busy_polls = cmd == 0x20 ? 5 : 20;
        if (cmd == 0x20) F.n_sector_erase++; else F.n_block_erase++;
        break;
    }
    case 0xB9:
        F.pd = 1;
        break;
    case 0x99:
        if (F.reset_en) { fake_reset_state(); F.n_reset++; }
        break;
    default:
        break;
    }
}

static void bus_select(void *ctx, bool active)
{
    (void)ctx;
    if (active) {
        assert(!F.selected && "select while already selected");
        F.selected = 1;
        F.pos = 0;
    } else {
        if (F.selected) fake_end_frame();
        F.selected = 0;
        F.pos = 0;
    }
}

static void bus_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
    (void)ctx;
    assert(F.selected && "transfer without /CS");
    for (size_t i = 0; i < len; i++) {
        uint8_t out = fake_byte(tx ? tx[i] : 0xFF);
        if (rx) rx[i] = out;
    }
}

static uint32_t bus_millis(void *ctx) { (void)ctx; return fake_ms++; }
static void bus_delay_us(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static const w25q_bus_t BUS = { NULL, bus_select, bus_transfer, bus_millis, bus_delay_us };

static void fake_new_chip(void)
{
    uint8_t *mem = F.mem;
    memset(&F, 0, sizeof F);
    F.mem = mem;
    memset(F.mem, 0xFF, W25Q_CAPACITY);
    F.jedec_dev = W25Q_ID_128_IQ;
}

/* --------------------------------- tests -------------------------------- */

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_init_ok(void)
{
    w25q_t d;
    fake_new_chip();
    F.pd = 1;                                    /* left in power-down by a previous run */
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);
    CHECK(d.manufacturer == 0xEF && d.device_id == 0x4018);
    CHECK(F.n_reset == 1);

    fake_new_chip();
    F.jedec_dev = W25Q_ID_128_IM;
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);

    fake_new_chip();
    F.busy_polls = 50;                           /* MCU reset during an erase */
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);
    CHECK(F.violations == 0);
}

static void test_init_errors(void)
{
    w25q_t d;
    fake_new_chip();
    F.jedec_dev = 0x4017;                        /* W25Q64, wrong chip */
    CHECK(w25q_init(&d, &BUS) == W25Q_ERR_ID);

    fake_new_chip();
    F.stuck_busy = 1;                            /* MISO stuck high / no chip */
    CHECK(w25q_init(&d, &BUS) == W25Q_ERR_TIMEOUT);

    CHECK(w25q_init(&d, NULL) == W25Q_ERR_PARAM);
}

static void test_write_read_pages(void)
{
    static uint8_t w[4096], r[4096];
    w25q_t d;
    const uint32_t base = 0x123000;
    fake_new_chip();
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);

    for (size_t i = 0; i < sizeof w; i++) w[i] = (uint8_t)(i * 31u + 7u);
    CHECK(w25q_erase_sector(&d, base) == W25Q_OK);
    CHECK(w25q_write(&d, base + 3, w, sizeof w - 3) == W25Q_OK);
    CHECK(F.n_program == 16);                    /* 4093 bytes from offset 3 = 16 pages */

    CHECK(w25q_read(&d, base + 3, r, sizeof w - 3) == W25Q_OK);
    CHECK(memcmp(r, w, sizeof w - 3) == 0);
    CHECK(F.mem[base] == 0xFF && F.mem[base + 2] == 0xFF);
    CHECK(F.mem[base + 0x1000] == 0xFF);        /* next sector untouched */
    CHECK(F.violations == 0);
}

static void test_write_verify_detects_unerased(void)
{
    uint8_t w[300];
    w25q_t d;
    fake_new_chip();
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);

    memset(w, 0xF0, sizeof w);
    CHECK(w25q_write_verify(&d, 0x2000, w, sizeof w) == W25Q_OK);
    memset(w, 0x0F, sizeof w);                   /* writing again without erase */
    CHECK(w25q_write_verify(&d, 0x2000, w, sizeof w) == W25Q_ERR_VERIFY);
    CHECK(F.violations == 0);
}

static void test_erase_range(void)
{
    w25q_t d;
    fake_new_chip();
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);

    /* One OTA slot: exactly two 64 KB blocks. */
    memset(F.mem + EXT_SLOT_DOWNLOAD, 0x00, EXT_SLOT_SIZE);
    CHECK(w25q_erase_range(&d, EXT_SLOT_DOWNLOAD, EXT_SLOT_SIZE) == W25Q_OK);
    CHECK(F.n_block_erase == 2 && F.n_sector_erase == 0);
    CHECK(F.mem[EXT_SLOT_DOWNLOAD] == 0xFF && F.mem[EXT_SLOT_DOWNLOAD + EXT_SLOT_SIZE - 1] == 0xFF);

    /* Unaligned start 0x11000..0x34000: 15 sectors, one block, then 4 sectors. */
    F.n_block_erase = F.n_sector_erase = 0;
    F.mem[0x0FFFF] = 0x00;                       /* just before the range */
    F.mem[0x34000] = 0x00;                       /* just after the range  */
    CHECK(w25q_erase_range(&d, 0x11000, 0x23000) == W25Q_OK);
    CHECK(F.n_sector_erase == 19 && F.n_block_erase == 1);
    CHECK(F.mem[0x0FFFF] == 0x00 && F.mem[0x34000] == 0x00);
    CHECK(F.violations == 0);
}

static void test_param_checks(void)
{
    uint8_t b[4];
    w25q_t d;
    fake_new_chip();
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);
    CHECK(w25q_erase_sector(&d, 0x1001) == W25Q_ERR_PARAM);
    CHECK(w25q_erase_block64(&d, 0x1000) == W25Q_ERR_PARAM);
    CHECK(w25q_erase_range(&d, 0x1000, 100) == W25Q_ERR_PARAM);
    CHECK(w25q_read(&d, W25Q_CAPACITY - 2, b, 4) == W25Q_ERR_PARAM);
    CHECK(w25q_write(&d, W25Q_CAPACITY, b, 1) == W25Q_ERR_PARAM);
    CHECK(w25q_read(&d, W25Q_CAPACITY - 4, b, 4) == W25Q_OK);
}

static void test_power_down_auto_wake(void)
{
    uint8_t w[16] = "stepwhere-ota!!", r[16];
    w25q_t d;
    fake_new_chip();
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);
    CHECK(w25q_write(&d, 0x500, w, sizeof w) == W25Q_OK);
    CHECK(w25q_power_down(&d) == W25Q_OK);
    CHECK(F.pd == 1);
    CHECK(w25q_read(&d, 0x500, r, sizeof r) == W25Q_OK);
    CHECK(F.pd == 0 && memcmp(r, w, sizeof w) == 0);
    CHECK(F.violations == 0);
}

static void test_write_protected(void)
{
    uint8_t w[4] = { 1, 2, 3, 4 };
    w25q_t d;
    fake_new_chip();
    CHECK(w25q_init(&d, &BUS) == W25Q_OK);
    F.write_protected = 1;
    CHECK(w25q_write(&d, 0, w, sizeof w) == W25Q_ERR_WEL);
    CHECK(w25q_erase_sector(&d, 0) == W25Q_ERR_WEL);
    CHECK(F.violations == 0);
}

static void test_layout(void)
{
    CHECK(sizeof(ota_image_header_t) == 256);
    CHECK(sizeof(ota_boot_meta_t) == 88);
    CHECK(APP_ADDR == 0x08004000UL);
    CHECK(APP_ADDR % MCU_FLASH_PAGE_SIZE == 0);
    CHECK(APP_MAX_SIZE + OTA_HDR_SIZE <= EXT_SLOT_SIZE);
    CHECK(EXT_SLOT_DOWNLOAD % EXT_BLOCK_SIZE == 0 && EXT_SLOT_BACKUP % EXT_BLOCK_SIZE == 0);
    CHECK(EXT_SLOT_FACTORY + EXT_SLOT_SIZE <= EXT_APP_DATA_ADDR);
    CHECK(OTA_VERSION(1, 2, 3) == 0x00010203UL);
    /* Offsets must match _HDR_FMT in tools/ota_tool.py */
    CHECK(offsetof(ota_image_header_t, fw_version) == 8);
    CHECK(offsetof(ota_image_header_t, image_size) == 12);
    CHECK(offsetof(ota_image_header_t, load_addr) == 16);
    CHECK(offsetof(ota_image_header_t, hw_id) == 20);
    CHECK(offsetof(ota_image_header_t, flags) == 24);
    CHECK(offsetof(ota_image_header_t, build_time) == 28);
    CHECK(offsetof(ota_image_header_t, payload_sha256) == 32);
    CHECK(offsetof(ota_image_header_t, git_sha1) == 64);
    CHECK(offsetof(ota_image_header_t, signature) == OTA_HDR_SIGNED_LEN);
    CHECK(offsetof(ota_image_header_t, hdr_crc32) == 252);
    /* "SWFW" as bytes in memory */
    uint32_t m = OTA_HDR_MAGIC;
    CHECK(memcmp(&m, "SWFW", 4) == 0);
}

int main(void)
{
    F.mem = malloc(W25Q_CAPACITY);
    if (!F.mem) return 2;

    test_layout();
    test_init_ok();
    test_init_errors();
    test_write_read_pages();
    test_write_verify_detects_unerased();
    test_erase_range();
    test_param_checks();
    test_power_down_auto_wake();
    test_write_protected();

    printf("%d checks, %d failed\n", g_checks, g_failed);
    free(F.mem);
    return g_failed ? 1 : 0;
}
