/*
 * accel_steps.cpp - see accel_steps.h.
 *
 * Interrupt use: TIM17 runs at the core's timer priority (3 on Cortex-M0+).
 * The Wire driver needs the I2C interrupt (priority 2) and SysTick
 * (priority 0) while it waits; both can interrupt TIM17, so Wire works from
 * this ISR. All LIS2DH12 I2C traffic after accel_steps_begin() happens in
 * this ISR only, so it never overlaps with another I2C user.
 */
#include "accel_steps.h"

#include <LIS2DH12Arduino.h>
#include <lis2dh12.h>
#include <step_counter.h>

#define ACCEL_HZ          25u
#define SERVICE_MS        500u     /* FIFO holds 32 samples = 1.28 s at 25 Hz  */
#define STALL_MS          3000u    /* no samples this long: set the sensor up again */
#define RETRY_MS          5000u    /* sensor missing: try again this often     */

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
    for (size_t i = 0; i < got; i++) steps += step_counter_push(&s_counter, buf[i].x, buf[i].y, buf[i].z);
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
