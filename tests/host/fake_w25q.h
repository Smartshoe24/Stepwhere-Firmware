/*
 * fake_w25q.h - Simulated W25Q128 for host tests, plus power-cut injection.
 *
 * The simulation follows the datasheet rules the driver relies on:
 *   - program/erase need WEL, and WEL clears after each one
 *   - only Read Status (05h) is accepted while BUSY
 *   - page program wraps inside its 256-byte page
 *   - programming can only clear bits (AND with old contents)
 *   - in deep power-down only Release (ABh) is accepted
 * Any rule the driver breaks is counted in F.violations.
 */
#ifndef FAKE_W25Q_H
#define FAKE_W25Q_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "w25q.h"

typedef struct {
    uint8_t *mem;
    uint16_t jedec_dev;        /* device ID to report                     */
    int      busy_polls;       /* SR reads left before BUSY clears        */
    int      stuck_busy;       /* never leave BUSY                        */
    int      write_protected;  /* 06h does not set WEL                    */
    int      wel, pd, reset_en;
    int      selected;
    size_t   pos;
    uint8_t  cmd;
    uint32_t addr;
    uint8_t  page[256];
    size_t   page_len;
    int      violations;
    int      n_sector_erase, n_block_erase, n_program, n_reset;
} fake_t;

extern fake_t F;
extern const w25q_bus_t FAKE_BUS;

/* Allocate the 16 MB array once; reset to an erased chip. */
void fake_new_chip(void);
/* Power cycle: chip loses volatile state (WEL, busy, power-down, /CS). */
void fake_power_cycle(void);

/* ---- power-cut injection, shared with the simulated MCU flash ---------- */
/* Set cut_countdown = N > 0: the N-th mutating operation (erase/program on
 * either flash) is cut half-way and longjmp(cut_env, 1) is called.
 * cut_countdown = 0 disables cuts. */
extern long    cut_countdown;
extern long    cut_ops;        /* mutating operations seen so far */
extern jmp_buf cut_env;
/* Called by a fake before a mutating op. Returns 1 if this op must be cut. */
int  cut_now(void);
void cut_fire(void);          /* longjmp out */

#endif /* FAKE_W25Q_H */
