/*
 * step_counter.h - Software step counter for a shoe-mounted accelerometer.
 *
 * The sensor sits in the heel of ONE shoe, so it sees one stride (two
 * steps) per cycle. Each stride of real walking looks like this:
 *
 *   stance (foot flat, almost no motion) -> swing (foot lifted and moved,
 *   a long burst of motion, ~0.25-0.6 s) -> heel strike -> stance ...
 *
 * The counter looks for exactly that pattern and is built to stay at zero
 * when a child sits and moves the foot:
 *
 *   1. motion = | |a| - g |: how far the length of the acceleration vector
 *      is from gravity, smoothed over ~120 ms. With the foot flat it is ~0
 *      whatever the orientation, so it does not depend on how the board
 *      sits in the heel. g is learned only while the foot is still.
 *   2. A "burst" is a stretch where motion > low_mg. It is a stride
 *      candidate only if
 *        - its peak reaches high_mg            (rejects small fidgets)
 *        - it lasts min_burst..max_burst ms    (taps are too short,
 *                                               shaking/vibration too long)
 *        - it carries min_energy of motion     (a heel lift + stomp while
 *                                               seated moves the foot far
 *                                               less than a swing)
 *        - the foot was still before it        (no stance = not walking)
 *   3. Candidates are counted only after confirm_strides of them arrive
 *      with a regular rhythm (interval min_stride..max_stride ms and within
 *      rhythm_pct % of the previous interval). Then all of them are counted
 *      at once. A pause longer than max_stride ends the walk.
 *
 * Integer-only (Cortex-M0+ has no FPU). About 100 bytes of state.
 * All tuning values live in step_counter_config_t so they can be adjusted
 * from recordings (tests/motion_host/step_replay).
 */
#ifndef STEPWHERE_STEP_COUNTER_H
#define STEPWHERE_STEP_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STEP_SMOOTH_MAX 16u   /* max smoothing window in samples */

typedef struct {
    uint16_t sample_hz;        /* rate of the samples pushed in (e.g. 25)        */
    uint16_t gravity_tau_ms;   /* time constant of the gravity estimate          */
    uint16_t smooth_ms;        /* moving-average window of the motion signal     */
    uint16_t low_mg;           /* motion above this = foot is moving             */
    uint16_t high_mg;          /* a stride burst must peak at least this high    */
    uint16_t merge_gap_ms;     /* dips shorter than this stay inside one burst   */
    uint16_t min_burst_ms;     /* shortest swing phase accepted                  */
    uint16_t max_burst_ms;     /* longest swing phase accepted                   */
    uint16_t min_energy_gms;   /* motion summed over the burst, in g x ms        */
    uint16_t min_still_ms;     /* foot must be still this long before a swing    */
    uint16_t min_stride_ms;    /* shortest time between strides of one foot      */
    uint16_t max_stride_ms;    /* longest time between strides (else walk ends)  */
    uint8_t  rhythm_pct;       /* allowed change between intervals while starting */
    uint8_t  confirm_strides;  /* regular strides needed before counting starts  */
    uint8_t  resume_strides;   /* ... if a walk ended less than resume_ms ago    */
    uint16_t resume_ms;        /* (turning, short stop): 0 = off                 */
    uint8_t  steps_per_stride; /* 2: sensor in one shoe only                     */
} step_counter_config_t;

typedef enum {
    STEP_STATE_IDLE = 0,       /* no regular walking seen                         */
    STEP_STATE_WALKING         /* counting                                        */
} step_state_t;

/* Why the last candidate burst was not accepted (for tuning). */
typedef enum {
    STEP_REJECT_NONE = 0,
    STEP_REJECT_SHORT,         /* burst shorter than min_burst                   */
    STEP_REJECT_LONG,          /* burst longer than max_burst                    */
    STEP_REJECT_ENERGY,        /* too little motion in the burst                 */
    STEP_REJECT_NOT_STILL      /* no stance (still foot) before the burst        */
} step_reject_t;

typedef struct {
    /* config converted to samples */
    step_counter_config_t cfg;
    uint8_t  gravity_shift;
    uint8_t  smooth_n;
    uint32_t merge_gap, min_burst, max_burst, min_still, min_stride, max_stride, resume;

    /* signal path */
    int32_t  gravity_q8;       /* |gravity| estimate, mg * 256 (starts at 1 g)   */
    int32_t  prev_mag;         /* |a| of the previous sample                     */
    uint8_t  still_samples;    /* samples in a row with almost no change of |a|  */
    uint16_t smooth_buf[STEP_SMOOTH_MAX];
    uint32_t smooth_sum;
    uint8_t  smooth_pos;
    uint8_t  smooth_fill;

    /* burst detector */
    uint32_t n;                /* samples pushed so far                          */
    bool     in_burst;
    uint32_t burst_start;
    uint32_t burst_last_high;  /* last sample with motion >= low_mg             */
    uint32_t still_before;     /* still samples before this burst               */
    uint32_t still_run;        /* current run of still samples                  */
    uint16_t burst_peak;
    uint32_t burst_sum;        /* sum of motion (mg) over the burst samples     */

    /* rhythm gate */
    step_state_t state;
    uint8_t  pending;          /* candidate strides waiting for confirmation     */
    bool     have_last;
    uint32_t last_stride;      /* sample index of the last accepted candidate    */
    uint32_t last_interval;    /* samples between the last two candidates        */
    bool     walked;           /* a walk has been counted before                 */
    uint32_t walk_end;         /* sample index of the last stride of that walk   */

    /* results */
    uint32_t strides;
    uint32_t steps;
    uint32_t rejected;         /* candidate bursts that failed a check          */
    uint16_t last_motion_mg;

    /* last finished burst that reached high_mg (for tuning / replay) */
    uint32_t last_burst_start;
    uint16_t last_burst_ms;
    uint16_t last_burst_peak_mg;
    uint16_t last_burst_energy_gms;
    step_reject_t last_reject;
} step_counter_t;

/* Defaults for the given sample rate. Starting values, to be tuned with
 * recordings from the real shoe. */
void step_counter_default_config(step_counter_config_t *cfg, uint16_t sample_hz);

/* Returns false if the config is unusable (e.g. sample_hz 0). */
bool step_counter_init(step_counter_t *sc, const step_counter_config_t *cfg);

/* Push one sample (milli-g). Returns the number of steps newly counted by
 * this sample (usually 0; steps_per_stride per stride; several at once
 * when a walk is confirmed). */
uint32_t step_counter_push(step_counter_t *sc, int16_t x_mg, int16_t y_mg, int16_t z_mg);

/* Lifetime total since init / reset. */
static inline uint32_t step_counter_steps(const step_counter_t *sc) { return sc->steps; }
static inline bool step_counter_walking(const step_counter_t *sc) { return sc->state == STEP_STATE_WALKING; }

/* Forget the walking state (e.g. after a gap in the data such as a FIFO
 * overrun). Keeps the totals. */
void step_counter_break(step_counter_t *sc);

/* Integer square root, exposed for tests. */
uint32_t step_isqrt(uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_STEP_COUNTER_H */
