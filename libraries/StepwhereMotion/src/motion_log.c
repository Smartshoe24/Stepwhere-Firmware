/*
 * motion_log.c - see motion_log.h.
 */
#include "motion_log.h"

#include <string.h>

uint16_t mlog_crc16(const uint8_t *p, uint32_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

static mlog_page_kind_t decode(const uint8_t *b, mlog_page_t *out)
{
    uint16_t magic = get16(b);
    if (magic == 0xFFFF) {
        for (unsigned i = 0; i < MLOG_PAGE_SIZE; i++)
            if (b[i] != 0xFF) return MLOG_PAGE_CORRUPT;
        return MLOG_PAGE_ERASED;
    }
    if (magic != MLOG_MAGIC || get16(b + 252) != mlog_crc16(b, 252)) return MLOG_PAGE_CORRUPT;
    if (b[3] == 0 || b[3] > MLOG_SAMPLES) return MLOG_PAGE_CORRUPT;
    if (out) {
        out->session = b[2];
        out->n = b[3];
        out->t0_ms = get32(b + 4);
        out->hz = get16(b + 8);
        for (unsigned i = 0; i < out->n; i++)
            for (unsigned k = 0; k < 3; k++) out->s[i][k] = (int16_t)get16(b + 12 + i * 6 + k * 2);
    }
    return MLOG_PAGE_VALID;
}

mlog_page_kind_t mlog_read_page(mlog_t *log, uint32_t idx, mlog_page_t *out)
{
    uint8_t b[MLOG_PAGE_SIZE];
    uint32_t addr = log->start + idx * MLOG_PAGE_SIZE;
    if (addr >= log->end || w25q_read(log->flash, addr, b, sizeof(b)) != W25Q_OK) return MLOG_PAGE_CORRUPT;
    return decode(b, out);
}

/* First two bytes only: enough to tell written from blank for the search. */
static int page_used(mlog_t *log, uint32_t idx, bool *used)
{
    uint8_t m[2];
    if (w25q_read(log->flash, log->start + idx * MLOG_PAGE_SIZE, m, 2) != W25Q_OK) return -1;
    *used = !(m[0] == 0xFF && m[1] == 0xFF);
    return 0;
}

mlog_status_t mlog_open(mlog_t *log, w25q_t *flash, uint32_t start, uint32_t end)
{
    if (!log || !flash) return MLOG_ERR_PARAM;
    if (!start && !end) { start = MLOG_START; end = MLOG_END; }
    if (start % MLOG_PAGE_SIZE || end % MLOG_PAGE_SIZE || end <= start) return MLOG_ERR_PARAM;
    memset(log, 0, sizeof(*log));
    log->flash = flash;
    log->start = start;
    log->end = end;

    /* Pages are written front to back, so used pages form one block from
     * the start: binary search for the first blank page. */
    uint32_t lo = 0, hi = (end - start) / MLOG_PAGE_SIZE;   /* answer in [lo, hi] */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        bool used;
        if (page_used(log, mid, &used)) return MLOG_ERR_FLASH;
        if (used) lo = mid + 1; else hi = mid;
    }
    log->wr = start + lo * MLOG_PAGE_SIZE;
    if (lo > 0) {
        mlog_page_t p;
        if (mlog_read_page(log, lo - 1, &p) == MLOG_PAGE_VALID) log->last_session = p.session;
        else log->last_session = 254;   /* unknown: the next session gets number 1 */
    }
    return MLOG_OK;
}

mlog_status_t mlog_begin(mlog_t *log, uint16_t hz)
{
    if (!log || hz == 0 || hz > 1000) return MLOG_ERR_PARAM;
    if (log->wr >= log->end) return MLOG_ERR_FULL;
    log->session = (uint8_t)(log->last_session >= 254 ? 1 : log->last_session + 1);
    log->last_session = log->session;
    log->hz = hz;
    log->next_t_ms = 0;
    log->buf.n = 0;
    return MLOG_OK;
}

mlog_status_t mlog_flush(mlog_t *log)
{
    if (!log || !log->session) return MLOG_ERR_PARAM;
    if (log->buf.n == 0) return MLOG_OK;
    if (log->wr >= log->end) return MLOG_ERR_FULL;

    uint8_t b[MLOG_PAGE_SIZE];
    /* The page must be blank: programming can only clear bits. A page cut
     * short by a power loss can look blank in its first bytes only; skip
     * such pages (a few at most) instead of failing. */
    for (unsigned tries = 0;; tries++) {
        if (log->wr >= log->end) return MLOG_ERR_FULL;
        if (w25q_read(log->flash, log->wr, b, sizeof(b)) != W25Q_OK) return MLOG_ERR_FLASH;
        bool blank = true;
        for (unsigned i = 0; i < MLOG_PAGE_SIZE && blank; i++) blank = (b[i] == 0xFF);
        if (blank) break;
        if (tries >= 16) return MLOG_ERR_NOT_ERASED;
        log->wr += MLOG_PAGE_SIZE;
    }

    memset(b, 0, sizeof(b));
    put16(b, MLOG_MAGIC);
    b[2] = log->session;
    b[3] = log->buf.n;
    put32(b + 4, log->buf.t0_ms);
    put16(b + 8, log->hz);
    for (unsigned i = 0; i < log->buf.n; i++)
        for (unsigned k = 0; k < 3; k++) put16(b + 12 + i * 6 + k * 2, (uint16_t)log->buf.s[i][k]);
    put16(b + 252, mlog_crc16(b, 252));
    if (w25q_write(log->flash, log->wr, b, sizeof(b)) != W25Q_OK) return MLOG_ERR_FLASH;
    log->wr += MLOG_PAGE_SIZE;
    log->buf.n = 0;
    return MLOG_OK;
}

mlog_status_t mlog_add(mlog_t *log, uint32_t t_ms, int16_t x, int16_t y, int16_t z)
{
    if (!log || !log->session) return MLOG_ERR_PARAM;
    if (log->wr >= log->end) return MLOG_ERR_FULL;
    /* A gap (lost samples) starts a new page so its time stays exact. */
    if (log->buf.n && t_ms != log->next_t_ms) {
        mlog_status_t st = mlog_flush(log);
        if (st != MLOG_OK) return st;
    }
    if (log->buf.n == 0) log->buf.t0_ms = t_ms;
    log->buf.s[log->buf.n][0] = x;
    log->buf.s[log->buf.n][1] = y;
    log->buf.s[log->buf.n][2] = z;
    log->buf.n++;
    uint32_t idx = (t_ms - log->buf.t0_ms) * log->hz / 1000u + 1u;   /* samples since t0 */
    log->next_t_ms = log->buf.t0_ms + (idx * 1000u + log->hz / 2u) / log->hz;
    if (log->buf.n == MLOG_SAMPLES) return mlog_flush(log);
    return MLOG_OK;
}

mlog_status_t mlog_end(mlog_t *log)
{
    if (!log) return MLOG_ERR_PARAM;
    mlog_status_t st = log->session ? mlog_flush(log) : MLOG_OK;
    log->session = 0;
    return st;
}

mlog_status_t mlog_erase(mlog_t *log, bool full, void (*progress)(void *ctx, uint32_t done, uint32_t total),
                         void *ctx)
{
    if (!log) return MLOG_ERR_PARAM;
    log->session = 0;
    log->buf.n = 0;
    uint32_t upto = full ? log->end : log->wr + MLOG_PAGE_SIZE;   /* one page past wr: may hold junk */
    if (upto > log->end) upto = log->end;
    uint32_t blocks = (upto - log->start + W25Q_BLOCK_SIZE - 1) / W25Q_BLOCK_SIZE;
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t a = log->start + i * W25Q_BLOCK_SIZE;
        w25q_status_t st = (a % W25Q_BLOCK_SIZE == 0 && a + W25Q_BLOCK_SIZE <= log->end)
                               ? w25q_erase_block64(log->flash, a)
                               : w25q_erase_range(log->flash, a, log->end - a < W25Q_BLOCK_SIZE ? log->end - a : W25Q_BLOCK_SIZE);
        if (st != W25Q_OK) return MLOG_ERR_FLASH;
        if (progress) progress(ctx, i + 1, blocks);
    }
    log->wr = log->start;
    log->last_session = 0;
    return MLOG_OK;
}

const char *mlog_status_str(mlog_status_t st)
{
    switch (st) {
    case MLOG_OK:             return "OK";
    case MLOG_ERR_FLASH:      return "SPI flash error";
    case MLOG_ERR_FULL:       return "log full (download with d, then erase with e)";
    case MLOG_ERR_NOT_ERASED: return "flash not blank (erase the whole log with E)";
    case MLOG_ERR_PARAM:      return "bad parameter";
    }
    return "unknown";
}
