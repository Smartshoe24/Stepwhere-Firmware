/*
 * step_debug.h - Statistics about what the step counter did with the
 * samples (for the STEPS_DEBUG log line and the host replay tools).
 *
 * Call step_debug_after_push() after every step_counter_push() with the
 * same sample. It only reads the counter's state, so step_counter.c is
 * unchanged:
 *   - in_burst going false            -> a burst ended
 *   - last_burst_start changing       -> that burst reached high_mg and was
 *                                        checked as a stride candidate
 *   - rejected growing in that call   -> it failed a check (last_reject)
 * Integer-only, no allocation; safe to call from an ISR.
 */
#ifndef STEPWHERE_STEP_DEBUG_H
#define STEPWHERE_STEP_DEBUG_H

#include <stdbool.h>
#include <stdint.h>

#include "step_counter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t samples;
    uint32_t still;            /* smoothed motion below low_mg             */
    uint32_t bursts;           /* bursts that ended                        */
    uint32_t candidates;       /* ... of them reached high_mg              */
    uint32_t passed;           /* ... and passed the single-stride checks */
    uint32_t rej[5];           /* candidates rejected, by step_reject_t    */
    uint32_t steps;            /* steps counted                            */
    uint32_t max_a_mg;         /* largest raw |a|                          */
    uint32_t max_motion_mg;    /* largest smoothed | |a| - g |             */
} step_debug_stats_t;

typedef struct {
    bool     in_burst;
    uint32_t last_start;
    uint32_t rejected;
} step_debug_track_t;

static inline void step_debug_track_init(step_debug_track_t *t, const step_counter_t *sc)
{
    t->in_burst = sc->in_burst;
    t->last_start = 0xFFFFFFFFu;
    t->rejected = sc->rejected;
}

static inline void step_debug_after_push(step_debug_stats_t *d, step_debug_track_t *t, const step_counter_t *sc,
                                         int16_t x_mg, int16_t y_mg, int16_t z_mg, uint32_t steps)
{
    int32_t x = x_mg, y = y_mg, z = z_mg;
    uint32_t a = step_isqrt((uint32_t)(x * x) + (uint32_t)(y * y) + (uint32_t)(z * z));
    if (a > d->max_a_mg) d->max_a_mg = a;
    if (sc->last_motion_mg > d->max_motion_mg) d->max_motion_mg = sc->last_motion_mg;
    if (sc->last_motion_mg < sc->cfg.low_mg) d->still++;
    d->samples++;
    d->steps += steps;

    if (t->in_burst && !sc->in_burst) d->bursts++;
    t->in_burst = sc->in_burst;

    if (sc->last_burst_start != t->last_start && sc->last_burst_peak_mg) {
        t->last_start = sc->last_burst_start;
        d->candidates++;
        if (sc->rejected != t->rejected && (unsigned)sc->last_reject < 5u)
            d->rej[sc->last_reject]++;
        else
            d->passed++;
    }
    t->rejected = sc->rejected;
}

/* Sum of the rejected candidates. */
static inline uint32_t step_debug_rejected(const step_debug_stats_t *d)
{
    return d->rej[1] + d->rej[2] + d->rej[3] + d->rej[4];
}

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_STEP_DEBUG_H */
