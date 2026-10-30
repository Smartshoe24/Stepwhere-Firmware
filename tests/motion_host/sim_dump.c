/*
 * sim_dump.c - Write a synthetic scenario from motion_sim.c as a CSV in the
 * same format as the recorder, so it can be fed to step_replay.
 *
 *   sim_dump SCENARIO [HZ] > out.csv
 *   SCENARIO: walk run slow child taps stomp jiggle dangle shake slowshake vehicle
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motion_sim.h"

static unsigned long g_n;
static int g_hz;

static void sink(void *ctx, int16_t x, int16_t y, int16_t z)
{
    (void)ctx;
    printf("%lu,%d,%d,%d\n", g_n * 1000UL / (unsigned long)g_hz, x, y, z);
    g_n++;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sim_dump SCENARIO [HZ]\n");
        return 2;
    }
    g_hz = argc > 2 ? atoi(argv[2]) : 50;
    sim_t s;
    sim_init(&s, (uint16_t)g_hz, sink, NULL);
    printf("#rec odr=%d synthetic %s\n", g_hz, argv[1]);
    sim_still(&s, 2);
    const char *k = argv[1];
    if (!strcmp(k, "walk")) sim_walk(&s, 50, 1.05, 0.40, 1.0);
    else if (!strcmp(k, "run")) sim_walk(&s, 50, 0.60, 0.60, 1.8);
    else if (!strcmp(k, "slow")) sim_walk(&s, 50, 1.60, 0.35, 0.7);
    else if (!strcmp(k, "child")) sim_walk(&s, 50, 0.80, 0.40, 0.9);
    else if (!strcmp(k, "taps")) sim_taps(&s, 60, 0.6, 250, 1500);
    else if (!strcmp(k, "stomp")) sim_taps(&s, 60, 1.0, 400, 3000);
    else if (!strcmp(k, "jiggle")) sim_sine(&s, 60, 4.5, 400, 2);
    else if (!strcmp(k, "dangle")) sim_dangle(&s, 60, 1.0, 0.5);
    else if (!strcmp(k, "shake")) sim_sine(&s, 30, 2.5, 1500, 0);
    else if (!strcmp(k, "slowshake")) sim_sine(&s, 30, 1.1, 1500, 0);
    else if (!strcmp(k, "vehicle")) sim_vehicle(&s, 120, 80, 600);
    else { fprintf(stderr, "unknown scenario %s\n", k); return 2; }
    sim_still(&s, 3);
    return 0;
}
