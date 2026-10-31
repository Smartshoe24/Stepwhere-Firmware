#include "fake_w25q.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

fake_t F;
long cut_countdown;
long cut_ops;
jmp_buf cut_env;
static uint32_t fake_ms;

int cut_now(void)
{
    cut_ops++;
    if (cut_countdown > 0 && --cut_countdown == 0) {
        return 1;
    }
    return 0;
}

void cut_fire(void)
{
    longjmp(cut_env, 1);
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
        if ((F.busy_polls > 0 || F.stuck_busy) && in != 0x05 && in != 0xAB && in != 0x9F) {
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
        if (F.busy_polls > 0 || F.stuck_busy) return 0xFF;   /* ignored while busy */
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
    case 0x02: {
        if (!F.wel) { F.violations++; break; }
        if (F.page_len > 256) { F.violations++; }   /* driver must not overflow a page */
        size_t n = F.page_len < 256 ? F.page_len : 256;
        int cut = cut_now();
        if (cut) n /= 2;                            /* power lost half-way */
        for (size_t i = 0; i < n; i++) {
            uint32_t a = (F.addr & 0xFFFF00u) | ((F.addr + (uint32_t)i) & 0xFFu);
            F.mem[a] &= F.page[i];
        }
        if (cut) cut_fire();
        F.wel = 0;
        F.busy_polls = 2;
        F.n_program++;
        break;
    }
    case 0x20:
    case 0xD8: {
        uint32_t size = cmd == 0x20 ? 4096u : 65536u;
        if (!F.wel) { F.violations++; break; }
        uint32_t base = F.addr & ~(size - 1u);
        if (cut_now()) {
            /* Interrupted erase: part erased, the rest left as garbage. */
            memset(F.mem + base, 0xFF, size / 2);
            memset(F.mem + base + size / 2, 0x5A, size / 2);
            cut_fire();
        }
        memset(F.mem + base, 0xFF, size);
        F.wel = 0;
        F.busy_polls = cmd == 0x20 ? 5 : 20;
        if (cmd == 0x20) F.n_sector_erase++; else F.n_block_erase++;
        break;
    }
    case 0xB9:
        F.pd = 1;
        break;
    case 0x99:
        if (F.reset_en) { F.wel = 0; F.reset_en = 0; F.n_reset++; }
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
        int was = F.selected;
        F.selected = 0;      /* clear first: fake_end_frame may longjmp */
        if (was) fake_end_frame();
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

const w25q_bus_t FAKE_BUS = { NULL, bus_select, bus_transfer, bus_millis, bus_delay_us };

void fake_new_chip(void)
{
    uint8_t *mem = F.mem;
    if (!mem) {
        mem = malloc(W25Q_CAPACITY);
        assert(mem);
    }
    memset(&F, 0, sizeof F);
    F.mem = mem;
    memset(F.mem, 0xFF, W25Q_CAPACITY);
    F.jedec_dev = W25Q_ID_128_IQ;
}

void fake_power_cycle(void)
{
    F.wel = 0;
    F.pd = 0;
    F.busy_polls = 0;
    F.reset_en = 0;
    F.selected = 0;
    F.pos = 0;
}
