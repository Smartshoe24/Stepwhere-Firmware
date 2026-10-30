/*
 * fake_lis2dh12.h - Register-level simulation of a LIS2DH12 on I2C, for the
 * host tests. Models WHO_AM_I, BOOT, auto-increment, output data format per
 * mode / full scale, BDU-free reads, FIFO bypass / stream with FSS, WTM and
 * OVRN, and records every register write.
 */
#ifndef FAKE_LIS2DH12_H
#define FAKE_LIS2DH12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lis2dh12.h"

#define FAKE_LOG_MAX 256

typedef struct {
    uint8_t  addr;               /* address the chip answers on (0x19)         */
    uint8_t  who_am_i;
    bool     nack;               /* every transfer fails (no chip / bus stuck) */
    uint8_t  reg[0x40];
    int16_t  fifo[32][3];        /* raw left-justified words                   */
    uint8_t  fifo_head, fifo_count;
    bool     fifo_ovrn;
    int16_t  current[3];         /* newest sample (FIFO off)                   */
    uint8_t  out_bytes_read;     /* bytes of the current sample already read   */
    int      n_boot;
    struct { uint8_t reg, val; } log[FAKE_LOG_MAX];
    int      n_log;
    int      n_reads, n_writes;
} fake_lis2dh12_t;

extern fake_lis2dh12_t FL;
extern const lis2dh12_bus_t FAKE_LIS_BUS;

void fake_lis_reset(void);

/* New sample from the sensing element, converted with the current mode and
 * full scale. Goes into the FIFO when it is enabled. */
void fake_lis_push_mg(int x_mg, int y_mg, int z_mg);

/* Last value written to reg, or -1 if never written. */
int fake_lis_last_write(uint8_t reg);

/* Index of the first write to reg in the log, or -1. */
int fake_lis_write_index(uint8_t reg, int from);

#endif
