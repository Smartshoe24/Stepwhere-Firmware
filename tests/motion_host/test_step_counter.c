/*
 * test_step_counter.c - Host tests for the software step counter.
 *
 * Uses the synthetic signals of motion_sim.c. Passing these tests means the
 * counter does what it was designed to do; it does not prove accuracy on a
 * real shoe. That needs recordings (tests/lis2dh12_bringup/README.md).
 *
 * Build and run:  tests/motion_host/run_tests.sh
 */
#include <stdio.h>
#include <stdlib.h>

#include "step_counter.h"
#include "motion_sim.h"

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_RANGE(v, lo, hi) do { g_checks++; long v_ = (long)(v); if (v_ < (lo) || v_ > (hi)) { \
    g_failed++; printf("  FAIL %s:%d: %s = %ld, expected %d..%d\n", __FILE__, __LINE__, #v, v_, (lo), (hi)); } } while (0)

typedef struct {
    step_counter_t sc;
    uint32_t steps;
} rig_t;

static void sink(void *ctx, int16_t x, int16_t y, int16_t z)
{
    rig_t *r = (rig_t *)ctx;
    r->steps += step_counter_push(&r->sc, x, y, z);
}

static void rig_init(rig_t *r, sim_t *s, uint16_t hz)
{
    step_counter_config_t c;
    step_counter_default_config(&c, hz);
    CHECK(step_counter_init(&r->sc, &c));
    r->steps = 0;
    sim_init(s, hz, sink, r);
    sim_still(s, 2.0);
}

/* ------------------------------------------------------------------ walking */

static void test_walk_speeds(uint16_t hz)
{
    static const struct { const char *name; double T, swing, amp; } w[] = {
        { "slow walk",   1.60, 0.35, 0.7 },
        { "normal walk", 1.05, 0.40, 1.0 },
        { "child walk",  0.80, 0.40, 0.9 },
        { "fast walk",   0.70, 0.42, 1.2 },
        { "running",     0.60, 0.60, 1.8 },
    };
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
        rig_t r; sim_t s;
        rig_init(&r, &s, hz);
        sim_walk(&s, 50, w[i].T, w[i].swing, w[i].amp);   /* 50 strides = 100 steps */
        sim_still(&s, 3.0);
        if (r.steps != 100)
            printf("  %s @%u Hz: %u steps (expected 100), rejected %u, last burst %u ms peak %u mg energy %u, reject %d\n",
                   w[i].name, hz, (unsigned)r.steps, (unsigned)r.sc.rejected, r.sc.last_burst_ms,
                   r.sc.last_burst_peak_mg, r.sc.last_burst_energy_gms, (int)r.sc.last_reject);
        CHECK_RANGE(r.steps, 98, 100);
        CHECK(step_counter_steps(&r.sc) == r.steps);
        CHECK(!step_counter_walking(&r.sc));                /* stopped after the pause */
    }
}

static void test_mounting_does_not_matter(void)
{
    static const double rot[][2] = { { 0, 0 }, { 1.2, 0 }, { 0, 1.57 }, { 2.5, 3.1 }, { -0.7, -2.0 } };
    for (size_t i = 0; i < sizeof(rot) / sizeof(rot[0]); i++) {
        rig_t r; sim_t s;
        rig_init(&r, &s, 25);
        s.yaw = rot[i][0];
        s.roll = rot[i][1];
        sim_still(&s, 3.0);
        sim_walk(&s, 50, 1.0, 0.4, 1.0);
        sim_still(&s, 3.0);
        CHECK_RANGE(r.steps, 98, 100);
    }
}

/* Sensor gain error: at rest |a| reads 0.85 g or 1.15 g instead of 1 g. */
typedef struct { rig_t r; double gain; } gain_rig_t;
static void gain_sink(void *ctx, int16_t x, int16_t y, int16_t z)
{
    gain_rig_t *g = (gain_rig_t *)ctx;
    g->r.steps += step_counter_push(&g->r.sc, (int16_t)(x * g->gain), (int16_t)(y * g->gain), (int16_t)(z * g->gain));
}

static void test_gain_error(void)
{
    static const double gains[] = { 0.85, 1.15 };
    for (size_t i = 0; i < 2; i++) {
        gain_rig_t g; sim_t s;
        step_counter_config_t c;
        step_counter_default_config(&c, 25);
        CHECK(step_counter_init(&g.r.sc, &c));
        g.r.steps = 0;
        g.gain = gains[i];
        sim_init(&s, 25, gain_sink, &g);
        sim_still(&s, 3.0);
        sim_walk(&s, 50, 1.0, 0.4, 1.0);
        sim_still(&s, 3.0);
        CHECK_RANGE(g.r.steps, 98, 100);
        CHECK_RANGE(g.r.sc.gravity_q8 / 256, (int)(1000 * gains[i]) - 30, (int)(1000 * gains[i]) + 30);
    }
}

static void test_confirmation(void)
{
    /* Fewer strides than confirm_strides are never counted (a child taking
     * 3 strides across the classroom gives 0); exactly that many counts all. */
    rig_t r; sim_t s;
    rig_init(&r, &s, 25);
    sim_walk(&s, 3, 1.0, 0.4, 1.0);
    sim_still(&s, 4.0);
    CHECK(r.steps == 0);
    sim_walk(&s, 4, 1.0, 0.4, 1.0);
    sim_still(&s, 4.0);
    CHECK(r.steps == 8);
}

static void test_pauses(void)
{
    rig_t r; sim_t s;
    rig_init(&r, &s, 25);
    sim_walk(&s, 20, 1.0, 0.4, 1.0);
    sim_still(&s, 5.0);
    sim_walk(&s, 20, 0.9, 0.4, 1.0);
    sim_still(&s, 5.0);
    CHECK_RANGE(r.steps, 78, 80);
}

static void test_speed_change_while_walking(void)
{
    rig_t r; sim_t s;
    rig_init(&r, &s, 25);
    sim_walk(&s, 20, 1.1, 0.4, 1.0);
    sim_walk(&s, 20, 0.75, 0.42, 1.2);
    sim_walk(&s, 20, 0.6, 0.6, 1.8);     /* starts running */
    sim_still(&s, 3.0);
    CHECK_RANGE(r.steps, 116, 120);
}

/* ------------------------------------------------- must stay at zero */

static void expect_zero(const char *name, rig_t *r)
{
    if (r->steps)
        printf("  %s: %u false steps, rejected %u, last burst %u ms peak %u mg energy %u, reject %d\n", name,
               (unsigned)r->steps, (unsigned)r->sc.rejected, r->sc.last_burst_ms, r->sc.last_burst_peak_mg,
               r->sc.last_burst_energy_gms, (int)r->sc.last_reject);
    CHECK(r->steps == 0);
}

static void test_sitting(uint16_t hz)
{
    rig_t r; sim_t s;

    /* Foot tapping, rhythmic, 5 minutes, light and hard taps. */
    static const struct { double period; int lift, stomp; } taps[] = {
        { 0.5, 150, 800 }, { 0.6, 250, 1500 }, { 0.8, 300, 2500 }, { 1.0, 400, 3000 },
    };
    for (size_t i = 0; i < sizeof(taps) / sizeof(taps[0]); i++) {
        rig_init(&r, &s, hz);
        sim_taps(&s, 300, taps[i].period, taps[i].lift, taps[i].stomp);
        expect_zero("foot tapping", &r);
    }

    /* Leg jiggling (3-6 Hz) and slower bouncing. */
    static const double jig[][2] = { { 4.5, 400 }, { 3.0, 600 }, { 6.0, 300 }, { 1.5, 500 } };
    for (size_t i = 0; i < sizeof(jig) / sizeof(jig[0]); i++) {
        rig_init(&r, &s, hz);
        sim_sine(&s, 300, jig[i][0], (int)jig[i][1], 2);
        expect_zero("leg jiggle", &r);
    }

    /* Foot dangling and swinging under the chair. */
    static const double dang[][2] = { { 1.0, 0.5 }, { 1.4, 0.7 }, { 0.8, 0.9 } };
    for (size_t i = 0; i < sizeof(dang) / sizeof(dang[0]); i++) {
        rig_init(&r, &s, hz);
        sim_dangle(&s, 300, dang[i][0], dang[i][1]);
        expect_zero("dangling foot", &r);
    }
}

static void test_vehicle_and_shaking(uint16_t hz)
{
    rig_t r; sim_t s;
    rig_init(&r, &s, hz);
    sim_vehicle(&s, 600, 80, 600);        /* 10 minutes on a bumpy road */
    expect_zero("vehicle", &r);

    rig_init(&r, &s, hz);
    sim_vehicle(&s, 600, 150, 1200);      /* rough road */
    expect_zero("rough vehicle", &r);

    for (int axis = 0; axis < 3; axis++) {
        rig_init(&r, &s, hz);
        sim_sine(&s, 60, 2.5, 1500, axis); /* shaking the shoe by hand */
        expect_zero("shaking", &r);
        rig_init(&r, &s, hz);
        sim_sine(&s, 60, 1.1, 1500, axis); /* slow shaking, walking rhythm */
        expect_zero("slow shaking", &r);
    }
}

static void test_fidget_then_walk(void)
{
    rig_t r; sim_t s;
    rig_init(&r, &s, 25);
    sim_taps(&s, 60, 0.6, 250, 1500);
    sim_walk(&s, 30, 1.0, 0.4, 1.0);
    sim_still(&s, 3.0);
    sim_sine(&s, 60, 4.5, 400, 2);
    sim_still(&s, 3.0);
    CHECK_RANGE(r.steps, 58, 60);
}

/* --------------------------------------------------------------- misc */

static void test_basics(void)
{
    CHECK(step_isqrt(0) == 0 && step_isqrt(1) == 1 && step_isqrt(15) == 3 && step_isqrt(16) == 4);
    CHECK(step_isqrt(1000000) == 1000 && step_isqrt(2700000000u) == 51961);
    CHECK(step_isqrt(0xFFFFFFFFu) == 65535);

    step_counter_t sc;
    step_counter_config_t c;
    step_counter_default_config(&c, 25);
    c.sample_hz = 0;
    CHECK(!step_counter_init(&sc, &c));
    step_counter_default_config(&c, 25);
    c.min_stride_ms = 3000;
    CHECK(!step_counter_init(&sc, &c));
    step_counter_default_config(&c, 25);
    CHECK(step_counter_init(&sc, &c));
    /* extreme input does not overflow */
    for (int i = 0; i < 200; i++) step_counter_push(&sc, (int16_t)(i & 1 ? 32767 : -32768), -32768, 32767);
    CHECK(sc.steps == 0);

    printf("step_counter_t: %u bytes\n", (unsigned)sizeof(step_counter_t));
}

int main(void)
{
    test_basics();
    test_walk_speeds(25);
    test_walk_speeds(50);
    test_mounting_does_not_matter();
    test_gain_error();
    test_confirmation();
    test_pauses();
    test_speed_change_while_walking();
    test_sitting(25);
    test_sitting(50);
    test_vehicle_and_shaking(25);
    test_vehicle_and_shaking(50);
    test_fidget_then_walk();
    printf("step_counter: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
