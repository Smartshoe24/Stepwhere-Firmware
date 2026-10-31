/*
 * step_counter.c - see step_counter.h for the method.
 */
#include "step_counter.h"

#include <string.h>

void step_counter_default_config(step_counter_config_t *cfg, uint16_t sample_hz)
{
    cfg->sample_hz = sample_hz;
    cfg->gravity_tau_ms = 1000;
    cfg->smooth_ms = 120;
    cfg->low_mg = 200;
    cfg->high_mg = 450;
    cfg->merge_gap_ms = 80;
    cfg->min_burst_ms = 200;
    cfg->max_burst_ms = 900;
    cfg->min_energy_gms = 200;    /* e.g. 400 ms at 0.5 g average                 */
    cfg->min_still_ms = 60;
    cfg->min_stride_ms = 450;     /* 267 steps/min: faster than a child sprinting */
    cfg->max_stride_ms = 2000;    /*  60 steps/min: slower than a slow walk       */
    cfg->rhythm_pct = 40;
    cfg->confirm_strides = 4;     /* = 8 steps before counting starts             */
    cfg->resume_strides = 2;      /* after turning / a short stop in a walk       */
    cfg->resume_ms = 10000;
    cfg->fill_turns = 1;
    cfg->steps_per_stride = 2;
}

uint32_t step_isqrt(uint32_t v)
{
    uint32_t res = 0, bit = 1UL << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static uint32_t ms_to_samples(uint32_t ms, uint16_t hz)
{
    uint32_t n = (ms * hz + 500u) / 1000u;
    return n ? n : 1u;
}

bool step_counter_init(step_counter_t *sc, const step_counter_config_t *cfg)
{
    if (!sc || !cfg || cfg->sample_hz == 0 || cfg->sample_hz > 1000 || cfg->confirm_strides == 0 ||
        cfg->min_stride_ms >= cfg->max_stride_ms || cfg->min_burst_ms > cfg->max_burst_ms ||
        cfg->low_mg > cfg->high_mg || cfg->steps_per_stride == 0 ||
        (cfg->resume_ms && (cfg->resume_strides == 0 || cfg->resume_strides > cfg->confirm_strides)))
        return false;

    memset(sc, 0, sizeof(*sc));
    sc->cfg = *cfg;
    sc->gravity_q8 = 1000 * 256;
    sc->prev_mag = 1000;

    /* EMA weight 1/2^k with 2^k >= tau in samples. */
    uint32_t tau = ms_to_samples(cfg->gravity_tau_ms, cfg->sample_hz);
    uint8_t k = 1;
    while (k < 12 && (1UL << k) < tau) k++;
    sc->gravity_shift = k;

    uint32_t sn = ms_to_samples(cfg->smooth_ms, cfg->sample_hz);
    sc->smooth_n = (uint8_t)(sn > STEP_SMOOTH_MAX ? STEP_SMOOTH_MAX : sn);

    sc->merge_gap = ms_to_samples(cfg->merge_gap_ms, cfg->sample_hz);
    sc->min_burst = ms_to_samples(cfg->min_burst_ms, cfg->sample_hz);
    sc->max_burst = ms_to_samples(cfg->max_burst_ms, cfg->sample_hz);
    sc->min_still = ms_to_samples(cfg->min_still_ms, cfg->sample_hz);
    sc->min_stride = ms_to_samples(cfg->min_stride_ms, cfg->sample_hz);
    sc->max_stride = ms_to_samples(cfg->max_stride_ms, cfg->sample_hz);
    sc->resume = cfg->resume_ms ? ms_to_samples(cfg->resume_ms, cfg->sample_hz) : 0;
    sc->state = STEP_STATE_IDLE;
    return true;
}

void step_counter_break(step_counter_t *sc)
{
    sc->state = STEP_STATE_IDLE;
    sc->pending = 0;
    sc->have_last = false;
    sc->last_interval = 0;
    sc->in_burst = false;
    sc->still_run = 0;
}

static uint32_t count(step_counter_t *sc, uint32_t strides)
{
    sc->walked = true;
    sc->walk_end = sc->last_stride;
    sc->strides += strides;
    uint32_t steps = strides * sc->cfg.steps_per_stride;
    sc->steps += steps;
    return steps;
}

/* A finished burst: decide whether it is a stride and run the rhythm gate. */
static uint32_t on_burst(step_counter_t *sc)
{
    uint32_t dur = sc->burst_last_high - sc->burst_start + 1u;
    if (sc->burst_peak < sc->cfg.high_mg) return 0;          /* small fidget: ignore */

    uint32_t energy = sc->burst_sum / sc->cfg.sample_hz;     /* mg x s = g x ms */
    uint32_t dur_ms = dur * 1000u / sc->cfg.sample_hz;
    sc->last_burst_start = sc->burst_start;
    sc->last_burst_ms = (uint16_t)(dur_ms > 0xFFFFu ? 0xFFFFu : dur_ms);
    sc->last_burst_peak_mg = sc->burst_peak;
    sc->last_burst_energy_gms = (uint16_t)(energy > 0xFFFFu ? 0xFFFFu : energy);

    step_reject_t why = STEP_REJECT_NONE;
    if (dur < sc->min_burst) why = STEP_REJECT_SHORT;
    else if (dur > sc->max_burst) why = STEP_REJECT_LONG;
    else if (energy < sc->cfg.min_energy_gms) why = STEP_REJECT_ENERGY;
    else if (sc->still_before < sc->min_still) why = STEP_REJECT_NOT_STILL;
    sc->last_reject = why;
    if (why != STEP_REJECT_NONE) {
        sc->rejected++;                                       /* not a walking stride */
        /* Short or weak bursts are ignored: real walking has one between
         * strides (push-off of the same foot, first recordings 2026-10-31),
         * so they must not reset the rhythm. Long continuous motion
         * (shaking, vibration) or motion with no stance does end a walk. */
        if (why == STEP_REJECT_LONG || why == STEP_REJECT_NOT_STILL) {
            uint32_t still = sc->still_run;
            step_counter_break(sc);
            sc->still_run = still;
        }
        return 0;
    }

    uint32_t t = sc->burst_start;
    if (!sc->have_last) {
        sc->have_last = true;
        sc->last_stride = t;
        sc->last_interval = 0;
        sc->pending = 1;
        sc->seq_start = t;
        return 0;
    }

    uint32_t iv = t - sc->last_stride;
    if (iv < sc->min_stride) {
        if (sc->state == STEP_STATE_WALKING) return 0;        /* extra burst inside a stride */
        sc->last_stride = t;                                  /* restart the sequence here */
        sc->last_interval = 0;
        sc->pending = 1;
        sc->seq_start = t;
        return 0;
    }
    if (iv > sc->max_stride) {                                /* normally caught by the timeout */
        sc->state = STEP_STATE_IDLE;
        sc->last_stride = t;
        sc->last_interval = 0;
        sc->pending = 1;
        sc->seq_start = t;
        return 0;
    }

    if (sc->state == STEP_STATE_WALKING) {
        sc->last_interval = iv;
        sc->last_stride = t;
        sc->walk_iv = iv;
        return count(sc, 1);
    }

    /* IDLE: build up a regular sequence. */
    uint32_t prev = sc->last_interval;
    uint32_t diff = iv > prev ? iv - prev : prev - iv;
    if (prev == 0 || diff * 100u <= prev * sc->cfg.rhythm_pct) {
        sc->pending++;
    } else {
        sc->pending = 2;                                      /* last two still form a pair */
        sc->seq_start = sc->last_stride;
    }
    sc->last_interval = iv;
    sc->last_stride = t;

    /* Right after a walk (turning around, a short stop) fewer regular
     * strides are needed to carry on counting. */
    uint32_t need = sc->cfg.confirm_strides;
    bool resumed = sc->resume && sc->walked && (sc->seq_start - sc->walk_end) <= sc->resume;
    if (resumed) need = sc->cfg.resume_strides;
    if (sc->pending >= need) {
        uint32_t n = sc->pending;
        /* Turning: the gap from the end of the walk to this sequence is
         * about two strides, i.e. one stride of this foot was a pivot with
         * no clear swing (recordings/2026-10-31). Count it. */
        if (resumed && sc->cfg.fill_turns && sc->walk_iv) {
            uint32_t gap = sc->seq_start - sc->walk_end;
            if (gap * 10u >= sc->walk_iv * 16u && gap * 10u <= sc->walk_iv * 27u) {
                n++;
                sc->turns++;
            }
        }
        sc->pending = 0;
        sc->state = STEP_STATE_WALKING;
        sc->walk_iv = iv;
        return count(sc, n);
    }
    return 0;
}

#define STILL_DELTA_MG 60   /* sample-to-sample change of |a| when still */
#define STILL_SAMPLES  3

static int32_t asr(int32_t v, uint8_t s)
{
    return v >= 0 ? (v >> s) : -((-v) >> s);
}

uint32_t step_counter_push(step_counter_t *sc, int16_t x_mg, int16_t y_mg, int16_t z_mg)
{
    /* |a|, each axis clamped to 30 g so the sum of squares fits 32 bits. */
    const int32_t a[3] = { x_mg, y_mg, z_mg };
    uint32_t sq = 0;
    for (int k = 0; k < 3; k++) {
        uint32_t u = (uint32_t)(a[k] < 0 ? -a[k] : a[k]);
        if (u > 30000u) u = 30000u;
        sq += u * u;
    }
    int32_t mag = (int32_t)step_isqrt(sq);

    /* With the foot flat (or the shoe at rest) |a| is gravity, whatever the
     * orientation. The estimate learns only while the foot is truly still:
     * |a| has changed by less than STILL_DELTA_MG for STILL_SAMPLES samples
     * in a row. That holds in every stance phase and never during a swing,
     * so walking does not pull it away from 1 g, and it absorbs the
     * sensor's offset and gain error even if the start value is wrong. */
    int32_t delta = mag - sc->prev_mag;
    if (delta < 0) delta = -delta;
    sc->prev_mag = mag;
    if (delta < STILL_DELTA_MG) {
        if (sc->still_samples < 0xFF) sc->still_samples++;
    } else {
        sc->still_samples = 0;
    }
    if (sc->still_samples >= STILL_SAMPLES)
        sc->gravity_q8 += asr(mag * 256 - sc->gravity_q8, sc->gravity_shift);
    int32_t dyn = mag - asr(sc->gravity_q8, 8);
    if (dyn < 0) dyn = -dyn;
    if (dyn > 0xFFFF) dyn = 0xFFFF;

    /* moving average */
    if (sc->smooth_fill == sc->smooth_n) sc->smooth_sum -= sc->smooth_buf[sc->smooth_pos];
    else sc->smooth_fill++;
    sc->smooth_buf[sc->smooth_pos] = (uint16_t)dyn;
    sc->smooth_sum += (uint32_t)dyn;
    sc->smooth_pos = (uint8_t)((sc->smooth_pos + 1u) % sc->smooth_n);
    uint16_t m = (uint16_t)(sc->smooth_sum / sc->smooth_fill);
    sc->last_motion_mg = m;

    uint32_t i = sc->n++;
    uint32_t steps = 0;

    if (!sc->in_burst) {
        if (m >= sc->cfg.low_mg) {
            sc->in_burst = true;
            sc->burst_start = i;
            sc->burst_last_high = i;
            sc->burst_peak = m;
            sc->burst_sum = m;
            sc->still_before = sc->still_run;
            sc->still_run = 0;
        } else if (sc->still_run < 0xFFFFFFFFu) {
            sc->still_run++;
        }
    } else if (m >= sc->cfg.low_mg) {
        sc->burst_last_high = i;
        if (m > sc->burst_peak) sc->burst_peak = m;
        if (sc->burst_sum < 0xFFFF0000u) sc->burst_sum += m;
    } else if (i - sc->burst_last_high >= sc->merge_gap) {
        sc->in_burst = false;
        sc->still_run = i - sc->burst_last_high;
        steps = on_burst(sc);
    }

    /* A pause ends the walk. */
    if (sc->have_last && !sc->in_burst && (i - sc->last_stride) > sc->max_stride) {
        sc->state = STEP_STATE_IDLE;
        sc->pending = 0;
        sc->have_last = false;
        sc->last_interval = 0;
    }
    return steps;
}
