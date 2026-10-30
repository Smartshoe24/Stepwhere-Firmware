/*
 * lis2dh12_bringup.ino - Checks the LIS2DH12 (IC7) on the GSM PCB and records
 * raw motion data for tuning the step counter.
 *
 * Flash this INSTEAD of the normal firmware at 0x08000000, open the debug
 * UART (USART1, PB6 TX / PB7 RX, 115200 baud). See README.md next to this file.
 *
 * At start-up it runs the board checks and prints RESULT: PASS / FAIL:
 *   1. I2C bus scan (SDA PA12, SCL PA11)
 *   2. WHO_AM_I = 0x33 at 0x19 (SA0 = PA1 HIGH) and at 0x18 (PA1 LOW)
 *   3. Samples arrive through the FIFO, |a| at rest is about 1 g
 *   4. INT1 (PB3) pulses on FIFO watermark
 *   5. INT2 (PB4) fires when the board is moved
 *
 * Keys (send over the UART):
 *   r  start recording: CSV "t_ms,x_mg,y_mg,z_mg" at 50 Hz, +-8 g, 12-bit,
 *      plus "#steps N" lines from the on-board step counter
 *   s  stop recording
 *   m  insert a "#MARK n" line (tools/record_steps.py sends it on Enter)
 *   c  reset the step count
 *   t  run the board checks again
 *   h  help
 *
 * Requires the StepwhereMotion library from libraries/StepwhereMotion.
 */
#include <HardwareSerial.h>
#include <Wire.h>
#include <LIS2DH12Arduino.h>
#include <lis2dh12.h>
#include <step_counter.h>

// ---------------------------------------------------------------------------
// Pins, traced from the Rev10 Gerbers (docs/STEPS_DECISIONS.md)
// ---------------------------------------------------------------------------
#define I2C_SDA    PA12  // MCU pin 34 <-> IC7 pin 4
#define I2C_SCL    PA11  // MCU pin 33  -> IC7 pin 1
#define ACC_SA0    PA1   // MCU pin 12  -> IC7 pin 3 (via 0R)
#define ACC_INT1   PB3   // MCU pin 42 <-  IC7 pin 12
#define ACC_INT2   PB4   // MCU pin 43 <-  IC7 pin 11
#define LDO_EN     PA10  // GSM/GPS LDO, kept off
#define FLASH_CS   PC6   // W25Q128 /CS, kept high (deselected)

#define FW_NAME "lis2dh12_bringup v0.1"

HardwareSerial SerialDBG(PB7, PB6);  // USART1 debug console (RX, TX)
TwoWire dev_i2c(I2C_SDA, I2C_SCL);
static LIS2DH12Arduino accel(dev_i2c, ACC_SA0, true);

static volatile uint32_t g_int1_edges = 0;
static volatile uint32_t g_int2_edges = 0;
static void isr_int1() { g_int1_edges++; }
static void isr_int2() { g_int2_edges++; }

static bool g_recording = false;
static uint32_t g_sample_index = 0;
static uint32_t g_marks = 0;
static uint32_t g_overruns = 0;
static step_counter_t g_steps;
static int32_t g_pair[3];
static bool g_pair_half = false;

static void print_status(const char *what, lis2dh12_status_t st)
{
    SerialDBG.print(what);
    SerialDBG.print(": ");
    SerialDBG.println(lis2dh12_status_str(st));
}

static void step_counter_reset()
{
    step_counter_config_t c;
    step_counter_default_config(&c, 25);   // fed with pairs of 50 Hz samples
    step_counter_init(&g_steps, &c);
    g_pair_half = false;
}

// ---------------------------------------------------------------------------
// Board checks
// ---------------------------------------------------------------------------
static int g_fail;

static void result(const char *step, bool ok, const char *detail = nullptr)
{
    SerialDBG.print(ok ? "[PASS] " : "[FAIL] ");
    SerialDBG.print(step);
    if (detail) {
        SerialDBG.print(" - ");
        SerialDBG.print(detail);
    }
    SerialDBG.println();
    if (!ok) g_fail++;
}

static void i2c_scan()
{
    SerialDBG.print("I2C devices:");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        dev_i2c.beginTransmission(a);
        if (dev_i2c.endTransmission() == 0) {
            SerialDBG.print(" 0x");
            SerialDBG.print(a, HEX);
            found++;
        }
    }
    if (!found) SerialDBG.print(" none");
    SerialDBG.println();
    result("I2C scan finds a device", found > 0,
           found ? nullptr : "check IC7 supply (series resistor near IC8), pull-ups R10/R23");
}

static lis2dh12_config_t record_config()
{
    lis2dh12_config_t c = {};
    c.odr = LIS2DH12_ODR_50HZ;
    c.mode = LIS2DH12_MODE_HIGH_RES;
    c.fs = LIS2DH12_FS_8G;
    c.fifo_watermark = 16;      // INT1 every 0.32 s
    c.int1_watermark = true;
    c.wake_threshold_mg = 0;
    c.int_active_low = true;
    return c;
}

static void run_checks()
{
    g_fail = 0;
    g_recording = false;
    SerialDBG.println();
    SerialDBG.println("=== Stepwhere LIS2DH12 board check (" FW_NAME ") ===");

    // 1. Bus scan with SA0 high
    digitalWrite(ACC_SA0, HIGH);
    delay(2);
    i2c_scan();

    // 2. WHO_AM_I at both addresses
    uint8_t id_hi = 0, id_lo = 0;
    bool ok_hi = accel.probe(LIS2DH12_ADDR_SA0_HIGH, &id_hi);
    digitalWrite(ACC_SA0, LOW);
    delay(2);
    bool ok_lo = accel.probe(LIS2DH12_ADDR_SA0_LOW, &id_lo);
    digitalWrite(ACC_SA0, HIGH);
    delay(2);
    char buf[64];
    snprintf(buf, sizeof(buf), "0x19 (PA1 high): %s 0x%02X", ok_hi ? "answers" : "no ACK", id_hi);
    result("WHO_AM_I = 0x33 at 0x19", ok_hi && id_hi == LIS2DH12_WHO_AM_I_VALUE, buf);
    snprintf(buf, sizeof(buf), "0x18 (PA1 low): %s 0x%02X", ok_lo ? "answers" : "no ACK", id_lo);
    result("SA0 follows PA1 (0x18 when low)", ok_lo && id_lo == LIS2DH12_WHO_AM_I_VALUE, buf);
    if (ok_hi && id_hi == 0x43) SerialDBG.println("       0x43 = LIS2DS12: this board has the OLD sensor");

    lis2dh12_status_t st = accel.begin();
    if (st != LIS2DH12_OK) {
        print_status("lis2dh12_init", st);
        result("driver init", false);
        SerialDBG.println("RESULT: FAILED");
        return;
    }

    // 3 + 4. FIFO data and INT1 watermark pulses
    lis2dh12_config_t c = record_config();
    st = lis2dh12_configure(accel.dev(), &c);
    result("configure 50 Hz, FIFO, INT1", st == LIS2DH12_OK, lis2dh12_status_str(st));
    g_int1_edges = 0;
    int32_t sum[3] = { 0, 0, 0 };
    uint32_t n = 0;
    uint32_t t0 = millis();
    // Read every 500 ms (25 samples): the FIFO passes the watermark of 16
    // each time, so INT1 must pulse about 4 times in 2 s.
    while (millis() - t0 < 2000) {
        delay(500);
        lis2dh12_sample_t s[32];
        size_t got = 0;
        if (lis2dh12_fifo_read(accel.dev(), s, 32, &got) != LIS2DH12_OK) break;
        for (size_t i = 0; i < got; i++) {
            sum[0] += s[i].x; sum[1] += s[i].y; sum[2] += s[i].z;
            n++;
        }
    }
    uint32_t edges = g_int1_edges;
    snprintf(buf, sizeof(buf), "%lu samples in 2 s (expect ~100)", (unsigned long)n);
    result("FIFO delivers samples", n >= 80 && n <= 120, buf);
    if (n) {
        int32_t ax = sum[0] / (int32_t)n, ay = sum[1] / (int32_t)n, az = sum[2] / (int32_t)n;
        uint32_t mag = step_isqrt((uint32_t)(ax * ax + ay * ay + az * az));
        snprintf(buf, sizeof(buf), "x %ld y %ld z %ld mg, |a| %lu mg (board must lie still)",
                 (long)ax, (long)ay, (long)az, (unsigned long)mag);
        result("|a| at rest 900..1100 mg", mag >= 900 && mag <= 1100, buf);
    }
    snprintf(buf, sizeof(buf), "%lu falling edges on PB3 (expect >= 3)", (unsigned long)edges);
    result("INT1 watermark interrupt", edges >= 3, buf);

    // 5. INT2 motion interrupt
    lis2dh12_config_t w = c;
    w.fifo_watermark = 0;
    w.int1_watermark = false;
    w.wake_threshold_mg = 250;
    w.wake_duration = 1;
    st = lis2dh12_configure(accel.dev(), &w);
    delay(200);
    g_int2_edges = 0;
    SerialDBG.println("       Keep still 2 s ...");
    delay(2000);
    uint32_t idle_edges = g_int2_edges;
    SerialDBG.println("       Now MOVE or tap the board (10 s) ...");
    t0 = millis();
    while (millis() - t0 < 10000 && g_int2_edges == idle_edges) delay(10);
    uint32_t moved = g_int2_edges - idle_edges;
    snprintf(buf, sizeof(buf), "%lu edges while still, %lu when moved", (unsigned long)idle_edges,
             (unsigned long)moved);
    result("INT2 motion interrupt", st == LIS2DH12_OK && moved > 0, buf);
    result("INT2 quiet when still", idle_edges == 0, nullptr);

    lis2dh12_power_down(accel.dev());
    SerialDBG.println(g_fail ? "RESULT: FAILED" : "RESULT: ALL CHECKS PASSED");
    SerialDBG.println("Keys: r = record, s = stop, m = mark, c = reset steps, t = checks, h = help");
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
static void start_recording()
{
    lis2dh12_config_t c = record_config();
    lis2dh12_status_t st = lis2dh12_configure(accel.dev(), &c);
    if (st != LIS2DH12_OK) {
        print_status("#ERROR configure", st);
        return;
    }
    step_counter_reset();
    g_sample_index = 0;
    g_marks = 0;
    g_overruns = 0;
    g_recording = true;
    SerialDBG.println("#rec odr=50 mode=hr fs=8g counter_hz=25 " FW_NAME);
    SerialDBG.println("#t_ms,x_mg,y_mg,z_mg");
}

static void stop_recording()
{
    if (!g_recording) return;
    g_recording = false;
    lis2dh12_power_down(accel.dev());
    SerialDBG.print("#stop samples=");
    SerialDBG.print(g_sample_index);
    SerialDBG.print(" steps=");
    SerialDBG.print(step_counter_steps(&g_steps));
    SerialDBG.print(" overruns=");
    SerialDBG.println(g_overruns);
}

static void service_recording()
{
    static uint32_t last_poll = 0;
    static uint32_t last_int1 = 0;
    // Read when INT1 fired (every 16 samples = 320 ms), and at least every
    // 450 ms in case an edge was missed (the FIFO is full after 640 ms).
    if (g_int1_edges == last_int1 && millis() - last_poll < 450) return;
    last_int1 = g_int1_edges;
    last_poll = millis();

    uint8_t level = 0;
    bool ovr = false;
    if (lis2dh12_fifo_level(accel.dev(), &level, &ovr) != LIS2DH12_OK) {
        SerialDBG.println("#ERROR I2C");
        return;
    }
    if (ovr) {
        g_overruns++;
        SerialDBG.println("#OVERRUN");   // data gap: the counter restarts its walk detection
        step_counter_break(&g_steps);
    }
    lis2dh12_sample_t s[32];
    size_t got = 0;
    if (lis2dh12_fifo_read(accel.dev(), s, 32, &got) != LIS2DH12_OK) return;

    uint32_t before = step_counter_steps(&g_steps);
    char line[40];
    for (size_t i = 0; i < got; i++) {
        snprintf(line, sizeof(line), "%lu,%d,%d,%d", (unsigned long)(g_sample_index * 20u), s[i].x, s[i].y, s[i].z);
        SerialDBG.println(line);
        g_sample_index++;
        if (!g_pair_half) {
            g_pair[0] = s[i].x; g_pair[1] = s[i].y; g_pair[2] = s[i].z;
            g_pair_half = true;
        } else {
            step_counter_push(&g_steps, (int16_t)((g_pair[0] + s[i].x) / 2), (int16_t)((g_pair[1] + s[i].y) / 2),
                              (int16_t)((g_pair[2] + s[i].z) / 2));
            g_pair_half = false;
        }
    }
    if (step_counter_steps(&g_steps) != before) {
        SerialDBG.print("#steps ");
        SerialDBG.println(step_counter_steps(&g_steps));
    }
}

static void help()
{
    SerialDBG.println("r = record CSV, s = stop, m = mark, c = reset steps, t = board checks, h = help");
}

void setup()
{
    pinMode(LDO_EN, OUTPUT);
    digitalWrite(LDO_EN, LOW);
    pinMode(FLASH_CS, OUTPUT);
    digitalWrite(FLASH_CS, HIGH);
    pinMode(ACC_SA0, OUTPUT);
    digitalWrite(ACC_SA0, HIGH);
    // Push-pull outputs of the sensor, configured active low: no pull needed.
    pinMode(ACC_INT1, INPUT);
    pinMode(ACC_INT2, INPUT);

    SerialDBG.begin(115200);
    delay(300);
    dev_i2c.begin();
    dev_i2c.setClock(400000);
    attachInterrupt(digitalPinToInterrupt(ACC_INT1), isr_int1, FALLING);
    attachInterrupt(digitalPinToInterrupt(ACC_INT2), isr_int2, FALLING);
    step_counter_reset();
    run_checks();
}

void loop()
{
    while (SerialDBG.available()) {
        int k = SerialDBG.read();
        switch (k) {
        case 'r': case 'R': if (!g_recording) start_recording(); break;
        case 's': case 'S': stop_recording(); break;
        case 'm': case 'M':
            g_marks++;
            SerialDBG.print("#MARK ");
            SerialDBG.print(g_marks);
            SerialDBG.print(" steps=");
            SerialDBG.println(step_counter_steps(&g_steps));
            break;
        case 'c': case 'C': step_counter_reset(); SerialDBG.println("#steps reset"); break;
        case 't': case 'T': stop_recording(); run_checks(); break;
        case 'h': case 'H': case '?': help(); break;
        default: break;
        }
    }
    if (g_recording) service_recording();
}
