/*
 * step_replay.c - Run the firmware step counter on a recording (PC tool).
 *
 *   step_replay [options] recording.csv
 *
 * The CSV comes from tests/lis2dh12_bringup (tools/record_steps.py):
 *   # comment lines, "#rec odr=50 ..." header, "#MARK n" markers
 *   t_ms,x_mg,y_mg,z_mg
 *
 * Options:
 *   --hz N          sample rate of the file (default: from the #rec header, else 50)
 *   --decimate N    average N samples into one (e.g. 50 Hz file -> 25 Hz counter)
 *   --set NAME=V    override a step_counter_config_t field, e.g. --set high_mg=500
 *   -v              print every candidate burst (accepted or why rejected)
 *
 * Prints the steps counted between markers and in total.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "step_counter.h"

static const char *k_reject[] = { "ok", "too short", "too long", "low energy", "no stance before" };

typedef struct { const char *name; size_t off; int is8; } field_t;
#define F16(n) { #n, offsetof(step_counter_config_t, n), 0 }
#define F8(n)  { #n, offsetof(step_counter_config_t, n), 1 }
static const field_t k_fields[] = {
    F16(gravity_tau_ms), F16(smooth_ms), F16(low_mg), F16(high_mg), F16(merge_gap_ms),
    F16(min_burst_ms), F16(max_burst_ms), F16(min_energy_gms), F16(min_still_ms),
    F16(min_stride_ms), F16(max_stride_ms), F8(rhythm_pct), F8(confirm_strides), F8(resume_strides), F16(resume_ms), F8(fill_turns), F8(steps_per_stride),
};

static int set_field(step_counter_config_t *c, const char *arg)
{
    const char *eq = strchr(arg, '=');
    if (!eq) return 0;
    size_t len = (size_t)(eq - arg);
    long v = strtol(eq + 1, NULL, 10);
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
        if (strlen(k_fields[i].name) == len && strncmp(k_fields[i].name, arg, len) == 0) {
            char *p = (char *)c + k_fields[i].off;
            if (k_fields[i].is8) *(uint8_t *)p = (uint8_t)v;
            else *(uint16_t *)p = (uint16_t)v;
            return 1;
        }
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: step_replay [--hz N] [--decimate N] [--set NAME=V]... [-v] file.csv\n");
    fprintf(stderr, "fields:");
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) fprintf(stderr, " %s", k_fields[i].name);
    fprintf(stderr, "\n");
    exit(2);
}

#define MAX_SETS 32

int main(int argc, char **argv)
{
    int hz = 0, dec = 1, verbose = 0, nsets = 0;
    const char *sets[MAX_SETS];
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hz") && i + 1 < argc) hz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--decimate") && i + 1 < argc) dec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < MAX_SETS) sets[nsets++] = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] == '-') usage();
        else path = argv[i];
    }
    if (!path || dec < 1) usage();
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }

    char line[256];
    step_counter_t sc;
    int started = 0;
    long acc[3] = { 0, 0, 0 };
    int nacc = 0;
    unsigned long samples = 0;
    unsigned long t_ms = 0;
    uint32_t since_mark = 0, last_burst_seen = 0xFFFFFFFFu, last_rejected = 0;
    const char *mark = "start";
    char mark_buf[64];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            const char *p = strstr(line, "odr=");
            if (p && !hz) hz = atoi(p + 4);
            if (started && (!strncmp(line, "#GAP", 4) || !strncmp(line, "#OVERRUN", 8))) {
                step_counter_break(&sc);       /* samples missing: restart walk detection */
                acc[0] = acc[1] = acc[2] = 0;
                nacc = 0;
                continue;
            }
            p = strstr(line, " steps=");
            if (!strncmp(line, "#end session", 12) && p) printf("(board counted %d steps)\n", atoi(p + 7));
            if (!strncmp(line, "#MARK", 5)) {
                if (started) printf("%-24s %6u steps\n", mark, (unsigned)since_mark);
                since_mark = 0;
                snprintf(mark_buf, sizeof(mark_buf), "after %.40s", line + 1);
                mark_buf[strcspn(mark_buf, "\r\n")] = 0;
                mark = mark_buf;
            }
            continue;
        }
        long x, y, z;
        if (sscanf(line, "%lu,%ld,%ld,%ld", &t_ms, &x, &y, &z) != 4) continue;
        if (!started) {
            if (!hz) hz = 50;
            if (hz % dec) { fprintf(stderr, "--decimate must divide %d\n", hz); return 2; }
            step_counter_config_t cfg;
            step_counter_default_config(&cfg, (uint16_t)(hz / dec));
            for (int i = 0; i < nsets; i++)
                if (!set_field(&cfg, sets[i])) { fprintf(stderr, "unknown field in --set %s\n", sets[i]); usage(); }
            if (!step_counter_init(&sc, &cfg)) { fprintf(stderr, "invalid configuration\n"); return 2; }
            printf("input %d Hz, counter %d Hz, high_mg=%u low_mg=%u min_burst=%u ms energy=%u gms\n",
                   hz, hz / dec, cfg.high_mg, cfg.low_mg, cfg.min_burst_ms, cfg.min_energy_gms);
            started = 1;
        }
        acc[0] += x; acc[1] += y; acc[2] += z;
        if (++nacc < dec) continue;
        uint32_t n = step_counter_push(&sc, (int16_t)(acc[0] / dec), (int16_t)(acc[1] / dec), (int16_t)(acc[2] / dec));
        acc[0] = acc[1] = acc[2] = 0;
        nacc = 0;
        samples++;
        since_mark += n;
        if (verbose && sc.last_burst_start != last_burst_seen && (sc.last_burst_peak_mg || sc.rejected != last_rejected)) {
            last_burst_seen = sc.last_burst_start;
            printf("  t=%7lu ms burst %4u ms peak %5u mg energy %4u gms: %s%s\n", t_ms, sc.last_burst_ms,
                   sc.last_burst_peak_mg, sc.last_burst_energy_gms, k_reject[sc.last_reject],
                   n ? " -> counted" : "");
        }
        last_rejected = sc.rejected;
    }
    fclose(f);
    if (!started) { fprintf(stderr, "no samples in %s\n", path); return 1; }
    printf("%-24s %6u steps\n", mark, (unsigned)since_mark);
    printf("TOTAL %u steps (%u strides, %u turns) in %.1f s, %u bursts rejected\n", (unsigned)sc.steps,
           (unsigned)sc.strides, (unsigned)sc.turns, (double)samples * dec / hz, (unsigned)sc.rejected);
    return 0;
}
