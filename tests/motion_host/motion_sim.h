/*
 * motion_sim.h - Synthetic accelerometer signals for the step counter tests.
 *
 * These are simple models, NOT recordings. They check that the counter
 * behaves as designed (walking counted, sitting fidgets rejected). Real
 * accuracy has to come from recordings of the shoe (see
 * tests/lis2dh12_bringup/README.md).
 *
 * Model of one stride of a heel-mounted sensor, period T:
 *   stance (foot flat, sensor noise only) for (1 - swing_frac) * T, then a
 *   swing of swing_frac * T: foot pitches up to ~35 deg (gravity rotates),
 *   accelerates forward then brakes (full sine on the forward axis), lifts
 *   (half sine up), then a heel-strike spike of ~30 ms.
 */
#ifndef MOTION_SIM_H
#define MOTION_SIM_H

#include <stdint.h>

typedef void (*sim_sink_t)(void *ctx, int16_t x, int16_t y, int16_t z);

typedef struct {
    uint16_t hz;          /* sample rate                                   */
    double   yaw, roll;   /* sensor mounting rotation (radians)             */
    uint32_t seed;        /* noise generator state                          */
    int      noise_mg;    /* +- uniform sensor noise                        */
    sim_sink_t sink;
    void    *ctx;
} sim_t;

void sim_init(sim_t *s, uint16_t hz, sim_sink_t sink, void *ctx);

/* Foot flat and still. */
void sim_still(sim_t *s, double seconds);

/* n strides with period T; swing_frac of the period is swing, amp scales
 * the swing accelerations (1.0 = normal walk). */
void sim_walk(sim_t *s, int n, double period_s, double swing_frac, double amp);

/* Seated heel taps: small lift (lift_mg) then a stomp spike (stomp_mg,
 * 40 ms), every period_s. */
void sim_taps(sim_t *s, double seconds, double period_s, int lift_mg, int stomp_mg);

/* Continuous sine on the vertical axis (leg jiggling, shaking). */
void sim_sine(sim_t *s, double seconds, double freq_hz, int amp_mg, int axis);

/* Foot dangling and swinging under a chair: pendulum, continuous rotation. */
void sim_dangle(sim_t *s, double seconds, double freq_hz, double angle_rad);

/* Vehicle: vibration noise plus random bumps. */
void sim_vehicle(sim_t *s, double seconds, int vib_mg, int bump_mg);

#endif
