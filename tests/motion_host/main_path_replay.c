/*
 * main_path_replay.c - Replay a 50 Hz recording through the MAIN firmware's
 * step path (PC tool, used by run_tests.sh).
 *
 *   main_path_replay recording.csv [phase]
 *
 * step_replay averages pairs of 50 Hz samples, like the bring-up sketch. The
 * main firmware (stepwhere/accel_steps.cpp) instead runs the sensor at a
 * native 25 Hz. This tool copies that path:
 *   - fake LIS2DH12 configured like step_config(): 25 Hz, normal mode
 *     (10-bit), +-8 g, FIFO stream, watermark 24, interrupts active low
 *   - every second recorded sample goes into the sensor (phase 0 = even,
 *     1 = odd samples), so the counter sees un-averaged 25 Hz data
 *   - the real lis2dh12.c FIFO code reads it every 12.5 samples (500 ms,
 *     like the TIM17 service), with the same overrun handling
 *   - samples go into step_counter with step_counter_default_config(25)
 * Prints one line: samples, overruns, steps, rejected bursts, gravity.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_lis2dh12.h"
#include "lis2dh12.h"
#include "step_counter.h"
#include "step_debug.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s recording.csv [phase 0|1]\n", argv[0]);
        return 2;
    }
    int phase = argc > 2 ? atoi(argv[2]) : 0;
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }

    fake_lis_reset();
    lis2dh12_t dev;
    if (lis2dh12_init(&dev, &FAKE_LIS_BUS, LIS2DH12_ADDR_SA0_HIGH) != LIS2DH12_OK) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    lis2dh12_config_t c;
    memset(&c, 0, sizeof c);
    c.odr = LIS2DH12_ODR_25HZ;
    c.mode = LIS2DH12_MODE_NORMAL;
    c.fs = LIS2DH12_FS_8G;
    c.fifo_watermark = 24;
    c.int_active_low = true;
    if (lis2dh12_configure(&dev, &c) != LIS2DH12_OK) {
        fprintf(stderr, "configure failed\n");
        return 1;
    }

    step_counter_config_t cfg;
    step_counter_default_config(&cfg, 25);
    step_counter_t sc;
    if (!step_counter_init(&sc, &cfg)) {
        fprintf(stderr, "counter init failed\n");
        return 1;
    }

    step_debug_stats_t dbg;
    memset(&dbg, 0, sizeof dbg);
    step_debug_track_t trk;
    step_debug_track_init(&trk, &sc);

    char line[256];
    unsigned long i = 0, pushed = 0, overruns = 0, reads = 0;
    unsigned since_poll = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        unsigned long t;
        long x, y, z;
        if (sscanf(line, "%lu,%ld,%ld,%ld", &t, &x, &y, &z) != 4) continue;
        if ((int)(i++ % 2u) != phase) continue;               /* sensor runs at 25 Hz */
        fake_lis_push_mg((int)x, (int)y, (int)z);
        if (++since_poll < 12u + (unsigned)(reads & 1u)) continue;  /* 500 ms = 12.5 samples */
        since_poll = 0;
        reads++;
        uint8_t level;
        bool ovr = false;
        lis2dh12_sample_t buf[LIS2DH12_FIFO_DEPTH];
        size_t got = 0;
        if (lis2dh12_fifo_level(&dev, &level, &ovr) != LIS2DH12_OK ||
            lis2dh12_fifo_read(&dev, buf, LIS2DH12_FIFO_DEPTH, &got) != LIS2DH12_OK) {
            fprintf(stderr, "bus error\n");
            return 1;
        }
        if (ovr) {
            overruns++;
            step_counter_break(&sc);
        }
        for (size_t k = 0; k < got; k++) {
            uint32_t n = step_counter_push(&sc, buf[k].x, buf[k].y, buf[k].z);
            step_debug_after_push(&dbg, &trk, &sc, buf[k].x, buf[k].y, buf[k].z, n);
        }
        pushed += got;
    }
    fclose(f);
    printf("TOTAL %u steps, %lu samples via FIFO, %lu overruns, %u bursts rejected, gravity %d mg\n",
           (unsigned)sc.steps, pushed, overruns, (unsigned)sc.rejected, (int)(sc.gravity_q8 / 256));
    /* Same numbers as the firmware's [STEPS] line (STEPS_DEBUG), whole file. */
    printf("STEPS_DEBUG samples %lu, still %lu%%, bursts %lu, candidates %lu: ok %lu, short %lu, long %lu, "
           "energy %lu, no-stance %lu | steps %lu | max |a| %lu mg, max motion %lu mg\n",
           (unsigned long)dbg.samples, (unsigned long)(dbg.samples ? dbg.still * 100u / dbg.samples : 0u),
           (unsigned long)dbg.bursts, (unsigned long)dbg.candidates, (unsigned long)dbg.passed,
           (unsigned long)dbg.rej[1], (unsigned long)dbg.rej[2], (unsigned long)dbg.rej[3], (unsigned long)dbg.rej[4],
           (unsigned long)dbg.steps, (unsigned long)dbg.max_a_mg, (unsigned long)dbg.max_motion_mg);
    /* The debug bookkeeping must agree with the counter's own totals. */
    if (dbg.steps != sc.steps || step_debug_rejected(&dbg) != sc.rejected || dbg.samples != pushed ||
        dbg.candidates != dbg.passed + step_debug_rejected(&dbg)) {
        fprintf(stderr, "STEPS_DEBUG bookkeeping does not match the counter\n");
        return 1;
    }
    return 0;
}
