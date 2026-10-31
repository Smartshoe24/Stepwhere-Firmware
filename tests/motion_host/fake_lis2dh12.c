/*
 * fake_lis2dh12.c - see fake_lis2dh12.h.
 */
#include "fake_lis2dh12.h"

#include <string.h>

fake_lis2dh12_t FL;

void fake_lis_reset(void)
{
    memset(&FL, 0, sizeof(FL));
    FL.addr = LIS2DH12_ADDR_SA0_HIGH;
    FL.who_am_i = LIS2DH12_WHO_AM_I_VALUE;
    FL.reg[LIS2DH12_CTRL_REG0] = 0x10;
    FL.reg[LIS2DH12_CTRL_REG1] = 0x07;
}

static int mode_of(void)
{
    if (FL.reg[LIS2DH12_CTRL_REG1] & LIS2DH12_CTRL1_LPEN) return LIS2DH12_MODE_LOW_POWER;
    if (FL.reg[LIS2DH12_CTRL_REG4] & LIS2DH12_CTRL4_HR) return LIS2DH12_MODE_HIGH_RES;
    return LIS2DH12_MODE_NORMAL;
}

static bool fifo_on(void)
{
    return (FL.reg[LIS2DH12_CTRL_REG5] & LIS2DH12_CTRL5_FIFO_EN) &&
           (FL.reg[LIS2DH12_FIFO_CTRL_REG] & 0xC0) != LIS2DH12_FIFO_MODE_BYPASS;
}

static int16_t to_raw(int mg)
{
    static const int sens[3][4] = { { 16, 32, 64, 192 }, { 4, 8, 16, 48 }, { 1, 2, 4, 12 } };
    static const int shift[3] = { 8, 6, 4 };
    int mode = mode_of();
    int fs = (FL.reg[LIS2DH12_CTRL_REG4] >> 4) & 3;
    int bits = 16 - shift[mode];
    int max = (1 << (bits - 1)) - 1, min = -(1 << (bits - 1));
    int v = mg / sens[mode][fs];
    if (v > max) v = max;
    if (v < min) v = min;
    return (int16_t)(v * (1 << shift[mode]));
}

void fake_lis_push_mg(int x_mg, int y_mg, int z_mg)
{
    int16_t s[3] = { to_raw(x_mg), to_raw(y_mg), to_raw(z_mg) };
    memcpy(FL.current, s, sizeof(s));
    if (!fifo_on()) return;
    if (FL.fifo_count == 32) {                  /* stream mode: drop oldest */
        FL.fifo_head = (uint8_t)((FL.fifo_head + 1) % 32);
        FL.fifo_count--;
        FL.fifo_ovrn = true;
    }
    memcpy(FL.fifo[(FL.fifo_head + FL.fifo_count) % 32], s, sizeof(s));
    FL.fifo_count++;
    /* Datasheet FIFO_SRC_REG: OVRN_FIFO goes high when the FIFO holds 32
     * unread samples; the next sample replaces the oldest. */
    if (FL.fifo_count == 32) FL.fifo_ovrn = true;
}

static uint8_t read_reg(uint8_t r)
{
    switch (r) {
    case LIS2DH12_WHO_AM_I:
        return FL.who_am_i;
    case LIS2DH12_FIFO_SRC_REG: {
        uint8_t fth = FL.reg[LIS2DH12_FIFO_CTRL_REG] & 0x1F;
        uint8_t v = 0;
        if (FL.fifo_count == 0) v |= LIS2DH12_FIFO_SRC_EMPTY;
        if (FL.fifo_ovrn) v |= LIS2DH12_FIFO_SRC_OVRN;
        if (FL.fifo_count > fth) v |= LIS2DH12_FIFO_SRC_WTM;
        /* FSS has 5 bits; with 32 samples only OVRN tells it is full. */
        v |= (uint8_t)(FL.fifo_count & 0x1F);
        return v;
    }
    case LIS2DH12_INT2_SRC: {
        uint8_t v = FL.reg[r];
        FL.reg[r] = 0;                          /* cleared by reading */
        return v;
    }
    default:
        break;
    }
    if (r >= LIS2DH12_OUT_X_L && r <= LIS2DH12_OUT_X_L + 5) {
        const int16_t *s = (fifo_on() && FL.fifo_count) ? FL.fifo[FL.fifo_head] : FL.current;
        int idx = r - LIS2DH12_OUT_X_L;
        uint16_t w = (uint16_t)s[idx / 2];
        uint8_t b = (idx & 1) ? (uint8_t)(w >> 8) : (uint8_t)(w & 0xFF);
        if (r == LIS2DH12_OUT_X_L + 5 && fifo_on() && FL.fifo_count) {
            FL.fifo_head = (uint8_t)((FL.fifo_head + 1) % 32);   /* pop on reading OUT_Z_H */
            FL.fifo_count--;
            FL.fifo_ovrn = false;
        }
        return b;
    }
    return FL.reg[r];
}

static void write_reg(uint8_t r, uint8_t v)
{
    if (FL.n_log < FAKE_LOG_MAX) {
        FL.log[FL.n_log].reg = r;
        FL.log[FL.n_log].val = v;
        FL.n_log++;
    }
    if (r == LIS2DH12_CTRL_REG5 && (v & LIS2DH12_CTRL5_BOOT)) {
        FL.n_boot++;
        v &= (uint8_t)~LIS2DH12_CTRL5_BOOT;     /* self-clearing */
    }
    if (r == LIS2DH12_FIFO_CTRL_REG && (v & 0xC0) == LIS2DH12_FIFO_MODE_BYPASS) {
        FL.fifo_count = 0;                      /* bypass empties the FIFO */
        FL.fifo_head = 0;
        FL.fifo_ovrn = false;
    }
    FL.reg[r] = v;
}

static bool bus_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    (void)ctx;
    FL.n_writes++;
    if (FL.nack || addr != FL.addr) return false;
    uint8_t r = reg & 0x7F;
    for (size_t i = 0; i < len; i++) {
        write_reg(r, data[i]);
        if (reg & LIS2DH12_AUTO_INC) r++;
    }
    return true;
}

static bool bus_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    (void)ctx;
    FL.n_reads++;
    if (FL.nack || addr != FL.addr) return false;
    if (len > 1 && !(reg & LIS2DH12_AUTO_INC)) return false;   /* driver must set it */
    uint8_t r = reg & 0x7F;
    for (size_t i = 0; i < len; i++) {
        data[i] = read_reg(r);
        r++;
    }
    return true;
}

static void bus_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

const lis2dh12_bus_t FAKE_LIS_BUS = { NULL, bus_write, bus_read, bus_delay };

int fake_lis_last_write(uint8_t reg)
{
    for (int i = FL.n_log - 1; i >= 0; i--)
        if (FL.log[i].reg == reg) return FL.log[i].val;
    return -1;
}

int fake_lis_write_index(uint8_t reg, int from)
{
    for (int i = from; i < FL.n_log; i++)
        if (FL.log[i].reg == reg) return i;
    return -1;
}
