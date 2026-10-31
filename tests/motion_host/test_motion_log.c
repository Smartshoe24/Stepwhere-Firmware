/*
 * test_motion_log.c - Host tests for the flash motion log, run against the
 * simulated W25Q128 from tests/host/fake_w25q.c.
 */
#include <stdio.h>
#include <string.h>

#include "motion_log.h"
#include "fake_w25q.h"

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define A_START 0x070000UL
#define A_END   0x090000UL   /* 128 KB = 512 pages, keeps the test fast */

static w25q_t g_flash;

static void fresh(void)
{
    fake_new_chip();
    CHECK(w25q_init(&g_flash, &FAKE_BUS) == W25Q_OK);
}

/* Record n samples at 50 Hz with values derived from the index. */
static void record(mlog_t *log, uint32_t n, uint32_t gap_at, uint32_t gap_ms)
{
    CHECK(mlog_begin(log, 50) == MLOG_OK);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = mlog_next_time(log);
        if (i == gap_at) t += gap_ms;
        CHECK(mlog_add(log, t, (int16_t)i, (int16_t)-(int32_t)i, (int16_t)(1000 + (i % 7))) == MLOG_OK);
    }
}

static void test_write_read(void)
{
    fresh();
    mlog_t log;
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK);
    CHECK(mlog_pages_used(&log) == 0 && log.last_session == 0);
    record(&log, 100, 1000, 0);                       /* 2 full pages + 20 */
    CHECK(mlog_pages_used(&log) == 2);
    CHECK(mlog_end(&log) == MLOG_OK);
    CHECK(mlog_pages_used(&log) == 3);
    CHECK(F.violations == 0);

    mlog_page_t p;
    CHECK(mlog_read_page(&log, 0, &p) == MLOG_PAGE_VALID);
    CHECK(p.session == 1 && p.n == 40 && p.t0_ms == 0 && p.hz == 50);
    CHECK(p.s[39][0] == 39 && p.s[39][1] == -39 && p.s[39][2] == 1000 + 39 % 7);
    CHECK(mlog_read_page(&log, 1, &p) == MLOG_PAGE_VALID && p.t0_ms == 800 && p.s[0][0] == 40);
    CHECK(mlog_read_page(&log, 2, &p) == MLOG_PAGE_VALID && p.n == 20 && p.t0_ms == 1600);
    CHECK(mlog_read_page(&log, 3, &p) == MLOG_PAGE_ERASED);

    /* Reopen after a reset: position and session are found again. */
    mlog_t log2;
    CHECK(mlog_open(&log2, &g_flash, A_START, A_END) == MLOG_OK);
    CHECK(mlog_pages_used(&log2) == 3 && log2.last_session == 1);
    record(&log2, 40, 1000, 0);
    CHECK(mlog_end(&log2) == MLOG_OK);
    CHECK(mlog_read_page(&log2, 3, &p) == MLOG_PAGE_VALID && p.session == 2 && p.t0_ms == 0);
}

static void test_gap_starts_new_page(void)
{
    fresh();
    mlog_t log;
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK);
    record(&log, 30, 10, 500);                        /* 500 ms lost after sample 9 */
    CHECK(mlog_end(&log) == MLOG_OK);
    mlog_page_t p;
    CHECK(mlog_read_page(&log, 0, &p) == MLOG_PAGE_VALID && p.n == 10 && p.t0_ms == 0);
    CHECK(mlog_read_page(&log, 1, &p) == MLOG_PAGE_VALID && p.n == 20 && p.t0_ms == 700 && p.s[0][0] == 10);
}

static void test_full_and_erase(void)
{
    fresh();
    mlog_t log;
    CHECK(mlog_open(&log, &g_flash, A_START, A_START + 4 * MLOG_PAGE_SIZE) == MLOG_OK);
    CHECK(mlog_begin(&log, 50) == MLOG_OK);
    mlog_status_t st = MLOG_OK;
    uint32_t i;
    for (i = 0; i < 1000 && st == MLOG_OK; i++) st = mlog_add(&log, mlog_next_time(&log), 1, 2, 3);
    CHECK(st == MLOG_ERR_FULL && i == 161);           /* 4 pages x 40, then full */
    CHECK(mlog_pages_used(&log) == 4);

    mlog_t big;
    CHECK(mlog_open(&big, &g_flash, A_START, A_END) == MLOG_OK && mlog_pages_used(&big) == 4);
    CHECK(mlog_erase(&big, false, NULL, NULL) == MLOG_OK);
    CHECK(mlog_pages_used(&big) == 0 && big.last_session == 0);
    mlog_page_t p;
    CHECK(mlog_read_page(&big, 0, &p) == MLOG_PAGE_ERASED);
    CHECK(mlog_open(&big, &g_flash, A_START, A_END) == MLOG_OK && mlog_pages_used(&big) == 0);
    CHECK(F.violations == 0);
}

static void test_dirty_area(void)
{
    /* Old data in the area (e.g. a board that was used for something else). */
    fresh();
    memset(F.mem + A_START, 0x00, 2 * 1024);
    mlog_t log;
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK);
    CHECK(mlog_pages_used(&log) == 8);                /* treated as used */
    CHECK(mlog_begin(&log, 50) == MLOG_OK);           /* last page invalid -> session 1 */
    CHECK(log.session == 1);

    /* Junk after the write position that does not show in the first bytes:
     * the page is skipped. */
    fresh();
    F.mem[A_START + 100] = 0x12;
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK && mlog_pages_used(&log) == 0);
    record(&log, 40, 1000, 0);
    CHECK(mlog_pages_used(&log) == 2);
    mlog_page_t p;
    CHECK(mlog_read_page(&log, 0, &p) == MLOG_PAGE_CORRUPT);
    CHECK(mlog_read_page(&log, 1, &p) == MLOG_PAGE_VALID && p.n == 40);

    /* Whole area dirty: full erase fixes it. */
    fresh();
    memset(F.mem + A_START, 0x00, A_END - A_START);
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK);
    CHECK(mlog_begin(&log, 50) == MLOG_ERR_FULL);
    CHECK(mlog_erase(&log, true, NULL, NULL) == MLOG_OK);
    CHECK(mlog_begin(&log, 50) == MLOG_OK);

    /* Nothing outside the area is touched. */
    CHECK(F.mem[A_START - 1] == 0xFF && F.mem[A_END] == 0xFF);
    CHECK(F.violations == 0);
}

static void test_session_wrap_and_params(void)
{
    fresh();
    mlog_t log;
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK);
    for (int s = 1; s <= 256; s++) {
        CHECK(mlog_begin(&log, 25) == MLOG_OK);
        CHECK(mlog_add(&log, 0, 0, 0, 1000) == MLOG_OK);
        CHECK(mlog_end(&log) == MLOG_OK);
    }
    CHECK(log.session == 0 && log.last_session == 2);   /* 1..254, then 1, 2 */
    CHECK(mlog_open(&log, &g_flash, A_START, A_END) == MLOG_OK && log.last_session == 2);
    CHECK(mlog_open(&log, &g_flash, A_START + 1, A_END) == MLOG_ERR_PARAM);
    CHECK(mlog_add(&log, 0, 0, 0, 0) == MLOG_ERR_PARAM);  /* no session */
    CHECK(mlog_begin(&log, 0) == MLOG_ERR_PARAM);
    CHECK(mlog_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
}

int main(void)
{
    test_write_read();
    test_gap_starts_new_page();
    test_full_and_erase();
    test_dirty_area();
    test_session_wrap_and_params();
    printf("motion_log: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
