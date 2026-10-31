/*
 * motion_sim.c - see motion_sim.h.
 */
#include "motion_sim.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void sim_init(sim_t *s, uint16_t hz, sim_sink_t sink, void *ctx)
{
    s->hz = hz;
    s->yaw = 0.0;
    s->roll = 0.0;
    s->seed = 12345u;
    s->noise_mg = 15;
    s->sink = sink;
    s->ctx = ctx;
}

static double rnd(sim_t *s)                     /* uniform -1..1 */
{
    s->seed = s->seed * 1103515245u + 12345u;
    return ((double)((s->seed >> 8) & 0xFFFF) / 32767.5) - 1.0;
}

/* Emit one sample given in shoe coordinates (x forward, y left, z up), in
 * mg, including gravity. Rotated into the sensor frame by roll then yaw. */
static void emit(sim_t *s, double fx, double fy, double fz)
{
    double y1 = fy * cos(s->roll) - fz * sin(s->roll);
    double z1 = fy * sin(s->roll) + fz * cos(s->roll);
    double x2 = fx * cos(s->yaw) - y1 * sin(s->yaw);
    double y2 = fx * sin(s->yaw) + y1 * cos(s->yaw);
    double n = s->noise_mg;
    s->sink(s->ctx, (int16_t)lrint(x2 + n * rnd(s)), (int16_t)lrint(y2 + n * rnd(s)),
            (int16_t)lrint(z1 + n * rnd(s)));
}

/* Gravity seen by a sensor pitched by angle a (toe up = positive). */
static void gravity(double a, double *gx, double *gz)
{
    *gx = 1000.0 * sin(a);
    *gz = 1000.0 * cos(a);
}

void sim_still(sim_t *s, double seconds)
{
    int n = (int)lrint(seconds * s->hz);
    for (int i = 0; i < n; i++) emit(s, 0, 0, 1000);
}

void sim_walk(sim_t *s, int strides, double T, double swing_frac, double amp)
{
    int per = (int)lrint(T * s->hz);
    int swing = (int)lrint(T * swing_frac * s->hz);
    int strike = (int)lrint(0.03 * s->hz);
    if (strike < 1) strike = 1;
    for (int k = 0; k < strides; k++) {
        for (int i = 0; i < per; i++) {
            double fx = 0, fy = 0, fz = 1000;
            int j = i - (per - swing);              /* position inside the swing */
            if (j >= 0) {
                double u = (double)j / swing;       /* 0..1 */
                double pitch = 0.6 * sin(M_PI * u); /* ~35 deg peak */
                double gx, gz;
                gravity(pitch, &gx, &gz);
                fx = gx + amp * 1400.0 * sin(2 * M_PI * u);
                fz = gz + amp * 700.0 * sin(M_PI * u);
                fy = amp * 150.0 * sin(2 * M_PI * u);
                if (j >= swing - strike) fz += amp * 2500.0;  /* heel strike */
            }
            emit(s, fx, fy, fz);
        }
    }
}

void sim_taps(sim_t *s, double seconds, double period, int lift_mg, int stomp_mg)
{
    int n = (int)lrint(seconds * s->hz);
    int per = (int)lrint(period * s->hz);
    int lift = (int)lrint(0.15 * s->hz);
    int stomp = (int)lrint(0.04 * s->hz);
    if (stomp < 1) stomp = 1;
    for (int i = 0; i < n; i++) {
        int j = i % per;
        double fz = 1000, fx = 0;
        if (j < lift) {
            double u = (double)j / lift;
            fz += lift_mg * sin(M_PI * u);
            fx += 0.3 * lift_mg * sin(2 * M_PI * u);
        } else if (j < lift + stomp) {
            fz += stomp_mg;
        }
        emit(s, fx, 0, fz);
    }
}

void sim_sine(sim_t *s, double seconds, double f, int amp, int axis)
{
    int n = (int)lrint(seconds * s->hz);
    for (int i = 0; i < n; i++) {
        double v = amp * sin(2 * M_PI * f * i / s->hz);
        emit(s, axis == 0 ? v : 0, axis == 1 ? v : 0, 1000 + (axis == 2 ? v : 0));
    }
}

void sim_dangle(sim_t *s, double seconds, double f, double angle)
{
    int n = (int)lrint(seconds * s->hz);
    double w = 2 * M_PI * f;
    for (int i = 0; i < n; i++) {
        double t = (double)i / s->hz;
        double a = angle * sin(w * t);
        double gx, gz;
        gravity(a, &gx, &gz);
        /* tangential acceleration of a ~0.4 m pendulum */
        double tang = -0.4 * angle * w * w * sin(w * t) * 1000.0 / 9.81;
        emit(s, gx + tang, 0, gz);
    }
}

void sim_vehicle(sim_t *s, double seconds, int vib, int bump)
{
    int n = (int)lrint(seconds * s->hz);
    int next = (int)lrint((0.5 + 2.5 * (rnd(s) + 1) / 2) * s->hz);
    int len = (int)lrint(0.1 * s->hz);
    int left = 0;
    for (int i = 0; i < n; i++) {
        double fz = 1000 + vib * rnd(s), fx = vib * rnd(s), fy = vib * rnd(s);
        if (--next <= 0) {
            left = len;
            next = (int)lrint((0.5 + 2.5 * (rnd(s) + 1) / 2) * s->hz);
        }
        if (left > 0) {
            fz += bump * sin(M_PI * (len - left) / len);
            left--;
        }
        emit(s, fx, fy, fz);
    }
}
