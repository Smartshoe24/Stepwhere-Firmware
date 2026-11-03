/*
 * accel_steps.cpp - see accel_steps.h.
 *
 * Interrupt use: TIM17 runs at the core's timer priority (3 on Cortex-M0+).
 * The Wire driver needs the I2C interrupt (priority 2) and SysTick
 * (priority 0) while it waits; both can interrupt TIM17, so Wire works from
 * this ISR. All LIS2DH12 I2C traffic after accel_steps_begin() happens in
 * this ISR only, so it never overlaps with another I2C user. The build
 * stops if the core's defaults change (#error below), and
 * accel_steps_check_irq_priorities() prints the real NVIC values at start.
 */
#include "accel_steps.h"

#include <string.h>

#include <LIS2DH12Arduino.h>
#include <lis2dh12.h>
#include <step_counter.h>
#include <step_debug.h>

#define ACCEL_HZ          25u
#define SERVICE_MS        500u     /* FIFO holds 32 samples = 1.28 s at 25 Hz  */
#define STALL_MS          3000u    /* no samples this long: set the sensor up again */
#define RETRY_MS          5000u    /* sensor missing: try again this often     */

/* Wire waits inside this ISR for the I2C interrupt and for SysTick (its
 * timeout uses HAL_GetTick). Both must preempt TIM17, i.e. have a lower
 * priority number. STM32duino 2.12.0 on Cortex-M0+: SysTick 0, I2C 2,
 * timers 3. Stop the build if a core update changes that. */
#if defined(I2C_IRQ_PRIO) && defined(TIM_IRQ_PRIO) && (I2C_IRQ_PRIO >= TIM_IRQ_PRIO)
#error "I2C_IRQ_PRIO must be lower (more urgent) than TIM_IRQ_PRIO: the FIFO is read with Wire from a timer ISR"
#endif
#if defined(TICK_INT_PRIORITY) && defined(TIM_IRQ_PRIO) && (TICK_INT_PRIORITY >= TIM_IRQ_PRIO)
#error "TICK_INT_PRIORITY must be lower (more urgent) than TIM_IRQ_PRIO: Wire's timeout needs SysTick inside a timer ISR"
#endif

static LIS2DH12Arduino *s_accel = nullptr;
static HardwareTimer *s_timer = nullptr;
static step_counter_t s_counter;

static volatile bool     s_ok = false;
static volatile uint32_t s_steps = 0;          /* counted since boot         */
static volatile uint32_t s_samples = 0;
static volatile uint32_t s_overruns = 0;
static volatile uint32_t s_i2c_errors = 0;
static volatile uint32_t s_restarts = 0;
static uint32_t s_last_sample_ms = 0;
static uint32_t s_last_retry_ms = 0;

#if STEPS_DEBUG
/* Statistics for one debug period, written only in the TIM17 ISR, with the
 * same code the host tools use (step_debug.h). */
static step_debug_stats_t s_dbg;
static step_debug_track_t s_dbg_track;
static uint32_t s_dbg_start_ms = 0;
#endif

static lis2dh12_config_t step_config()
{
    lis2dh12_config_t c = {};
    c.odr = LIS2DH12_ODR_25HZ;
    c.mode = LIS2DH12_MODE_NORMAL;     /* 10-bit is plenty for steps     */
    c.fs = LIS2DH12_FS_8G;             /* heel strikes exceed 4 g        */
    c.fifo_watermark = 24;             /* INT1 not routed yet            */
    c.int1_watermark = false;
    c.wake_threshold_mg = 0;
    c.int_active_low = true;           /* idle high, matches the old pins */
    return c;
}

static bool sensor_setup()
{
    lis2dh12_t *d = s_accel->dev();
    lis2dh12_status_t st = lis2dh12_init(d, &d->bus, s_accel->address());
    if (st == LIS2DH12_OK) {
        lis2dh12_config_t c = step_config();
        st = lis2dh12_configure(d, &c);
    }
    s_ok = (st == LIS2DH12_OK);
    s_last_sample_ms = millis();
    return s_ok;
}

static void service()
{
    uint32_t now = millis();
    if (!s_ok) {
        if (now - s_last_retry_ms >= RETRY_MS) {
            s_last_retry_ms = now;
            if (sensor_setup()) {
                s_restarts++;
                step_counter_break(&s_counter);
            }
        }
        return;
    }

    lis2dh12_t *d = s_accel->dev();
    uint8_t level = 0;
    bool ovr = false;
    lis2dh12_sample_t buf[LIS2DH12_FIFO_DEPTH];
    size_t got = 0;
    if (lis2dh12_fifo_level(d, &level, &ovr) != LIS2DH12_OK ||
        lis2dh12_fifo_read(d, buf, LIS2DH12_FIFO_DEPTH, &got) != LIS2DH12_OK) {
        s_i2c_errors++;
        got = 0;
    }
    if (got == 0) {
        /* Sensor reset or lost power: it comes back powered down. */
        if (now - s_last_sample_ms >= STALL_MS) {
            step_counter_break(&s_counter);
            if (sensor_setup()) s_restarts++;
            else s_last_retry_ms = now;
        }
        return;
    }
    s_last_sample_ms = now;
    if (ovr) {
        s_overruns++;
        step_counter_break(&s_counter);   /* samples lost: restart walk detection */
    }

    uint32_t steps = 0;
    for (size_t i = 0; i < got; i++) {
        uint32_t n = step_counter_push(&s_counter, buf[i].x, buf[i].y, buf[i].z);
        steps += n;
#if STEPS_DEBUG
        step_debug_after_push(&s_dbg, &s_dbg_track, &s_counter, buf[i].x, buf[i].y, buf[i].z, n);
#endif
    }
    s_samples += got;
    if (steps) {
        s_steps += steps;
        accel_steps_on_steps(steps);
    }
}

bool accel_steps_begin(TwoWire &wire, uint32_t sa0_pin)
{
    static LIS2DH12Arduino accel(wire, sa0_pin, true);
    s_accel = &accel;

    step_counter_config_t cfg;
    step_counter_default_config(&cfg, ACCEL_HZ);
    step_counter_init(&s_counter, &cfg);

    lis2dh12_status_t st = accel.begin();       /* pins, Wire, WHO_AM_I */
    if (st == LIS2DH12_OK) sensor_setup();
    else s_ok = false;
    s_last_retry_ms = millis();
#if STEPS_DEBUG
    step_debug_track_init(&s_dbg_track, &s_counter);
    s_dbg_start_ms = millis();
#endif

    s_timer = new HardwareTimer(TIM17);
    s_timer->setOverflow(SERVICE_MS * 1000UL, MICROSEC_FORMAT);
    s_timer->attachInterrupt(service);
    s_timer->resume();
    return s_ok;
}

void accel_steps_print_status(Print &out)
{
    noInterrupts();
    bool ok = s_ok;
    uint32_t steps = s_steps, samples = s_samples, ovr = s_overruns, err = s_i2c_errors, rst = s_restarts;
    interrupts();
    out.print("[ACCEL] LIS2DH12 ");
    out.print(ok ? "OK" : "NOT RESPONDING");
    out.print(", steps since boot ");
    out.print(steps);
    out.print(", samples ");
    out.print(samples);
    out.print(", overruns ");
    out.print(ovr);
    out.print(", I2C errors ");
    out.print(err);
    out.print(", restarts ");
    out.println(rst);
}

bool accel_steps_check_irq_priorities(Print &out)
{
    /* Actual NVIC values, so a setInterruptPriority() call elsewhere is
     * caught too. dev_i2c (PA12/PA11) is I2C2 on the STM32G070. */
    uint32_t tim = NVIC_GetPriority(TIM17_IRQn);
    uint32_t tick = NVIC_GetPriority(SysTick_IRQn);
    uint32_t i2c = NVIC_GetPriority(I2C2_IRQn);
    bool ok = (i2c < tim) && (tick < tim);
    out.print("[ACCEL] IRQ priorities: SysTick ");
    out.print(tick);
    out.print(", I2C2 ");
    out.print(i2c);
    out.print(", TIM17 ");
    out.print(tim);
    out.println(ok ? " -> OK (I2C and SysTick preempt the FIFO timer)"
                   : " -> ERROR: I2C/SysTick cannot preempt TIM17, FIFO reads will time out");
    return ok;
}

#if STEPS_DEBUG
static const char *const k_reject_name[5] = { "none", "short", "long", "energy", "no-stance" };

void accel_steps_print_debug(Print &out)
{
    noInterrupts();
    step_debug_stats_t d = s_dbg;
    memset(&s_dbg, 0, sizeof(s_dbg));
    uint32_t start_ms = s_dbg_start_ms;
    s_dbg_start_ms = millis();
    int32_t g = s_counter.gravity_q8 / 256;
    bool walking = step_counter_walking(&s_counter);
    uint8_t pending = s_counter.pending;
    uint16_t low = s_counter.cfg.low_mg, high = s_counter.cfg.high_mg;
    interrupts();

    uint32_t rejected = step_debug_rejected(&d), main_rej = 0;
    for (unsigned i = 1; i < 5; i++) {
        if (d.rej[i] > d.rej[main_rej]) main_rej = i;
    }
    char line[220];
    snprintf(line, sizeof(line),
             "[STEPS] %lus: %lu samples, still %lu%% | bursts %lu (%lu below %u mg) | candidates %lu: ok %lu, "
             "short %lu, long %lu, energy %lu, no-stance %lu | main reject: %s",
             (unsigned long)((millis() - start_ms) / 1000u), (unsigned long)d.samples,
             (unsigned long)(d.samples ? d.still * 100u / d.samples : 0u), (unsigned long)d.bursts,
             (unsigned long)(d.bursts > d.candidates ? d.bursts - d.candidates : 0u), (unsigned)high,
             (unsigned long)d.candidates, (unsigned long)d.passed, (unsigned long)d.rej[1], (unsigned long)d.rej[2],
             (unsigned long)d.rej[3], (unsigned long)d.rej[4], rejected ? k_reject_name[main_rej] : "-");
    out.println(line);
    snprintf(line, sizeof(line),
             "[STEPS]   steps %lu | max |a| %lu mg, max motion %lu mg (burst > %u, stride peak >= %u) | "
             "gravity %ld mg | %s, pending %u",
             (unsigned long)d.steps, (unsigned long)d.max_a_mg, (unsigned long)d.max_motion_mg, (unsigned)low,
             (unsigned)high, (long)g, walking ? "walking" : "idle", (unsigned)pending);
    out.println(line);
}
#endif
