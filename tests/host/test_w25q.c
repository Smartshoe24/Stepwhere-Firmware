/*
 * test_w25q.c - Host unit tests for the w25q driver.
 *
 * Runs the real driver code against the simulated W25Q128 in fake_w25q.c.
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
#include "fake_w25q.h"

#define BUS FAKE_BUS

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
    fake_new_chip();

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
    return g_failed ? 1 : 0;
}
