/*
 * lis2dh12_bringup.ino - Checks the LIS2DH12 (IC7) on the GSM PCB and records
 * motion data for tuning the step counter, WITHOUT a cable in the shoe.
 *
 * Flash this INSTEAD of the normal firmware at 0x08000000. Debug UART:
 * USART1, PB6 TX / PB7 RX, 115200 baud. See README.md next to this file.
 *
 * After power-up / reset:
 *   1. Board check (prints [PASS]/[FAIL] lines and RESULT).
 *   2. Starts a new recording session in the W25Q128 external flash:
 *      50 Hz, 12-bit, +-8 g, about 13 hours of space. The cable can be
 *      unplugged; the board keeps recording on battery.
 *   Every power-up or reset starts a new session.
 *
 * Keys (with the cable connected; while recording, keys that stop the
 * recording or erase need a 'y' within 5 s to confirm):
 *   i  info: sessions, space used, live step count
 *   d  stop recording and download all sessions as CSV
 *      (use tools/record_steps.py dump, it saves one file per session)
 *   n  end this session and start a new one
 *   x  stop recording
 *   e  erase the recorded sessions (E = erase the whole log area)
 *   t  full board check, including the INT2 motion test (move the board)
 *   r  live CSV over the UART (desk use; m = marker)
 *   h  help
 *
 * Requires libraries/StepwhereMotion and libraries/StepwhereOTA.
 */
#include <HardwareSerial.h>
#include <IWatchdog.h>
#include <Wire.h>
#include <LIS2DH12Arduino.h>
#include <W25QArduino.h>
#include <lis2dh12.h>
#include <motion_log.h>
#include <step_counter.h>
#include <w25q.h>

// ---------------------------------------------------------------------------
// Pins. Accelerometer traced from the Rev10 Gerbers (docs/STEPS_DECISIONS.md),
// flash from bootloader/src/board.h.
// ---------------------------------------------------------------------------
#define I2C_SDA    PA12  // MCU pin 34 <-> IC7 pin 4
#define I2C_SCL    PA11  // MCU pin 33  -> IC7 pin 1
#define ACC_SA0    PA1   // MCU pin 12  -> IC7 pin 3 (via 0R)
#define ACC_INT1   PB3   // MCU pin 42 <-  IC7 pin 12
#define ACC_INT2   PB4   // MCU pin 43 <-  IC7 pin 11
#define FLASH_MOSI PA7
#define FLASH_MISO PA6
#define FLASH_SCK  PA5
#define FLASH_CS   PC6
#define FLASH_WP   PC7
#define FLASH_HOLD PB12
#define LDO_EN     PA10  // GSM/GPS LDO, kept off

#define FW_NAME "lis2dh12_bringup v0.3"
#define REC_HZ  50u
#define SAMPLE_MS (1000u / REC_HZ)

HardwareSerial SerialDBG(PB7, PB6);  // USART1 debug console (RX, TX)
TwoWire dev_i2c(I2C_SDA, I2C_SCL);
static LIS2DH12Arduino accel(dev_i2c, ACC_SA0, true);
static W25QArduino flash(FLASH_MOSI, FLASH_MISO, FLASH_SCK, FLASH_CS, FLASH_WP, FLASH_HOLD);

static volatile uint32_t g_int1_edges = 0;
static volatile uint32_t g_int2_edges = 0;
static void isr_int1() { g_int1_edges++; }
static void isr_int2() { g_int2_edges++; }

enum Mode { MODE_IDLE, MODE_FLASH, MODE_LIVE };
static Mode g_mode = MODE_IDLE;
static bool g_accel_ok = false;
static bool g_flash_ok = false;
static mlog_t g_log;

// live step counter (25 Hz, fed with averaged pairs of 50 Hz samples)
static step_counter_t g_steps;
static int32_t g_pair[3];
static bool g_pair_half = false;

// recording state
static uint32_t g_sess_start_ms = 0;
static uint32_t g_live_index = 0;
static uint32_t g_marks = 0;
static uint32_t g_overruns = 0;
static uint32_t g_last_status = 0;
static uint32_t g_last_sample_ms = 0;   // last time the FIFO gave samples
static uint32_t g_restarts = 0;         // sensor restarts after a stall
static uint32_t g_i2c_errors = 0;
static bool g_resync = false;           // next batch: place it by the clock

// Why the MCU started (RCC_CSR flags, read once at boot, then cleared).
static void print_reset_cause()
{
    uint32_t csr = RCC->CSR;
    SerialDBG.print("Reset cause:");
    if (csr & RCC_CSR_LPWRRSTF) SerialDBG.print(" low-power");
    if (csr & RCC_CSR_WWDGRSTF) SerialDBG.print(" window-watchdog");
    if (csr & RCC_CSR_IWDGRSTF) SerialDBG.print(" WATCHDOG (firmware hung)");
    if (csr & RCC_CSR_SFTRSTF)  SerialDBG.print(" software");
    if (csr & RCC_CSR_PWRRSTF)  SerialDBG.print(" POWER (power-up or brown-out)");
    if (csr & RCC_CSR_PINRSTF)  SerialDBG.print(" reset-pin");
    if (csr & RCC_CSR_OBLRSTF)  SerialDBG.print(" option-bytes");
    SerialDBG.print("  (CSR ");
    SerialDBG.print(csr >> 24, HEX);
    SerialDBG.println(")");
    RCC->CSR |= RCC_CSR_RMVF;
}

static void steps_reset()
{
    step_counter_config_t c;
    step_counter_default_config(&c, REC_HZ / 2);
    step_counter_init(&g_steps, &c);
    g_pair_half = false;
}

static void steps_push(const lis2dh12_sample_t &s)
{
    if (!g_pair_half) {
        g_pair[0] = s.x; g_pair[1] = s.y; g_pair[2] = s.z;
        g_pair_half = true;
        return;
    }
    step_counter_push(&g_steps, (int16_t)((g_pair[0] + s.x) / 2), (int16_t)((g_pair[1] + s.y) / 2),
                      (int16_t)((g_pair[2] + s.z) / 2));
    g_pair_half = false;
}

static void steps_gap()
{
    step_counter_break(&g_steps);
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

static void check_accel(bool with_int2)
{
    char buf[80];

    // I2C scan
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
           found ? nullptr : "check the IC7 supply (3.3VL) and pull-ups R10/R23");

    // WHO_AM_I at both addresses
    uint8_t id_hi = 0, id_lo = 0;
    bool ok_hi = accel.probe(LIS2DH12_ADDR_SA0_HIGH, &id_hi);
    digitalWrite(ACC_SA0, LOW);
    delay(2);
    bool ok_lo = accel.probe(LIS2DH12_ADDR_SA0_LOW, &id_lo);
    digitalWrite(ACC_SA0, HIGH);
    delay(2);
    snprintf(buf, sizeof(buf), "0x19 (PA1 high): %s 0x%02X", ok_hi ? "answers" : "no ACK", id_hi);
    result("WHO_AM_I = 0x33 at 0x19", ok_hi && id_hi == LIS2DH12_WHO_AM_I_VALUE, buf);
    snprintf(buf, sizeof(buf), "0x18 (PA1 low): %s 0x%02X", ok_lo ? "answers" : "no ACK", id_lo);
    result("SA0 follows PA1 (0x18 when low)", ok_lo && id_lo == LIS2DH12_WHO_AM_I_VALUE, buf);
    if (ok_hi && id_hi == 0x43) SerialDBG.println("       0x43 = LIS2DS12: this board has the OLD sensor");

    lis2dh12_status_t st = accel.begin();
    g_accel_ok = (st == LIS2DH12_OK);
    if (!g_accel_ok) {
        result("driver init", false, lis2dh12_status_str(st));
        return;
    }

    // FIFO data and INT1 watermark pulses
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

    if (with_int2) {
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
        while (millis() - t0 < 10000 && g_int2_edges == idle_edges) {
            delay(10);
            IWatchdog.reload();
        }
        uint32_t moved = g_int2_edges - idle_edges;
        snprintf(buf, sizeof(buf), "%lu edges while still, %lu when moved", (unsigned long)idle_edges,
                 (unsigned long)moved);
        result("INT2 motion interrupt", st == LIS2DH12_OK && moved > 0, buf);
        result("INT2 quiet when still", idle_edges == 0, nullptr);
    }
    lis2dh12_power_down(accel.dev());
}

static void check_flash()
{
    char buf[80];
    w25q_status_t st = flash.begin();
    if (st == W25Q_OK) {
        mlog_status_t ms = mlog_open(&g_log, flash.dev(), 0, 0);
        g_flash_ok = (ms == MLOG_OK);
        snprintf(buf, sizeof(buf), "W25Q128 ID %02X %04X, log %s", flash.dev()->manufacturer, flash.dev()->device_id,
                 mlog_status_str(ms));
    } else {
        g_flash_ok = false;
        snprintf(buf, sizeof(buf), "%s", w25q_status_str(st));
    }
    result("external flash for the log", g_flash_ok, buf);
    if (g_flash_ok && mlog_pages_used(&g_log) > 0) {
        static mlog_page_t p;
        if (mlog_read_page(&g_log, 0, &p) != MLOG_PAGE_VALID)
            SerialDBG.println("       The log area holds old data. Erase it once: send E, then y (takes 1-8 min).");
    }
}

static void print_info()
{
    if (!g_flash_ok) {
        SerialDBG.println("#info flash not available");
        return;
    }
    uint32_t used = mlog_pages_used(&g_log), total = mlog_pages_total(&g_log);
    uint32_t free_min = (total - used) * MLOG_SAMPLES / REC_HZ / 60u;
    char buf[160];
    snprintf(buf, sizeof(buf), "#info %s, session %u, log %lu/%lu pages (%lu.%lu%%), %lu h %lu min left, steps %lu, "
             "up %lu s, sensor restarts %lu, I2C errors %lu",
             g_mode == MODE_FLASH ? "RECORDING" : (g_mode == MODE_LIVE ? "live" : "stopped"),
             g_log.session ? g_log.session : g_log.last_session, (unsigned long)used, (unsigned long)total,
             (unsigned long)(used * 100u / total), (unsigned long)(used * 1000u / total % 10u),
             (unsigned long)(free_min / 60u), (unsigned long)(free_min % 60u),
             (unsigned long)step_counter_steps(&g_steps), (unsigned long)(millis() / 1000u),
             (unsigned long)g_restarts, (unsigned long)g_i2c_errors);
    SerialDBG.println(buf);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
static bool start_sensor()
{
    if (!g_accel_ok) return false;
    lis2dh12_config_t c = record_config();
    lis2dh12_status_t st = lis2dh12_configure(accel.dev(), &c);
    if (st != LIS2DH12_OK) {
        SerialDBG.print("#ERROR configure: ");
        SerialDBG.println(lis2dh12_status_str(st));
        return false;
    }
    steps_reset();
    g_sess_start_ms = millis();
    g_last_sample_ms = millis();
    g_overruns = 0;
    g_resync = false;
    return true;
}

static void start_flash_recording()
{
    if (!g_accel_ok || !g_flash_ok) {
        SerialDBG.println("#Not recording: accelerometer or flash failed the check.");
        return;
    }
    mlog_status_t ms = mlog_begin(&g_log, REC_HZ);
    if (ms != MLOG_OK) {
        SerialDBG.print("#Not recording: ");
        SerialDBG.println(mlog_status_str(ms));
        return;
    }
    if (!start_sensor()) return;
    g_mode = MODE_FLASH;
    SerialDBG.print("#RECORDING session ");
    SerialDBG.print(g_log.session);
    SerialDBG.println(" to flash. You can unplug the cable now.");
    print_info();
}

static void stop_recording()
{
    if (g_mode == MODE_FLASH) {
        mlog_status_t ms = mlog_end(&g_log);
        SerialDBG.print("#stopped session ");
        SerialDBG.print(g_log.last_session);
        SerialDBG.print(", steps ");
        SerialDBG.print(step_counter_steps(&g_steps));
        SerialDBG.print(", overruns ");
        SerialDBG.print(g_overruns);
        SerialDBG.print(", ");
        SerialDBG.println(mlog_status_str(ms));
    } else if (g_mode == MODE_LIVE) {
        SerialDBG.print("#stop samples=");
        SerialDBG.print(g_live_index);
        SerialDBG.print(" steps=");
        SerialDBG.print(step_counter_steps(&g_steps));
        SerialDBG.print(" overruns=");
        SerialDBG.println(g_overruns);
    }
    if (g_mode != MODE_IDLE && g_accel_ok) lis2dh12_power_down(accel.dev());
    g_mode = MODE_IDLE;
}

static void start_live()
{
    if (!start_sensor()) return;
    g_live_index = 0;
    g_marks = 0;
    g_mode = MODE_LIVE;
    SerialDBG.println("#rec odr=50 mode=hr fs=8g counter_hz=25 " FW_NAME);
    SerialDBG.println("#t_ms,x_mg,y_mg,z_mg");
}

// The FIFO gave no samples for 2 s (it should give 50 per second). Report
// what the sensor says, then set it up again. CTRL_REG1 = 00 means the
// sensor lost its configuration, i.e. it was reset or lost power.
static void sensor_stalled()
{
    lis2dh12_t *d = accel.dev();
    uint8_t who = 0, c1 = 0, fs = 0;
    bool ok = lis2dh12_read_reg(d, LIS2DH12_WHO_AM_I, &who) == LIS2DH12_OK &&
              lis2dh12_read_reg(d, LIS2DH12_CTRL_REG1, &c1) == LIS2DH12_OK &&
              lis2dh12_read_reg(d, LIS2DH12_FIFO_SRC_REG, &fs) == LIS2DH12_OK;
    char buf[120];
    snprintf(buf, sizeof(buf), "#WARN no samples for %lu ms at up %lu s: %s WHO_AM_I %02X CTRL_REG1 %02X FIFO_SRC %02X",
             (unsigned long)(millis() - g_last_sample_ms), (unsigned long)(millis() / 1000u),
             ok ? "I2C ok," : "I2C FAILED,", who, c1, fs);
    SerialDBG.println(buf);
    g_restarts++;
    steps_gap();
    g_resync = true;
    g_last_sample_ms = millis();
    lis2dh12_config_t c = record_config();
    lis2dh12_status_t st = lis2dh12_init(d, &d->bus, d->addr);
    if (st == LIS2DH12_OK) st = lis2dh12_configure(d, &c);
    SerialDBG.print("#sensor restart: ");
    SerialDBG.println(lis2dh12_status_str(st));
}

static void service_sensor()
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
    lis2dh12_sample_t s[32];
    size_t got = 0;
    if (lis2dh12_fifo_level(accel.dev(), &level, &ovr) != LIS2DH12_OK ||
        lis2dh12_fifo_read(accel.dev(), s, 32, &got) != LIS2DH12_OK) {
        g_i2c_errors++;
        if (g_i2c_errors < 20 || g_i2c_errors % 100 == 0) {
            SerialDBG.print("#ERROR I2C, count ");
            SerialDBG.println(g_i2c_errors);
        }
        got = 0;
    }
    if (got == 0) {
        if (millis() - g_last_sample_ms > 2000) sensor_stalled();
        return;
    }
    g_last_sample_ms = millis();

    uint32_t t;
    if (g_mode == MODE_FLASH) {
        t = mlog_next_time(&g_log);
        if (ovr || g_resync) {
            // Samples were lost: place this batch by the clock, on the 20 ms grid.
            uint32_t now = millis() - g_sess_start_ms;
            uint32_t est = now > (got - 1) * SAMPLE_MS ? now - (got - 1) * SAMPLE_MS : 0;
            est = est / SAMPLE_MS * SAMPLE_MS;
            if (est > t) t = est;
        }
    } else {
        t = g_live_index * SAMPLE_MS;
    }
    g_resync = false;
    if (ovr) {
        g_overruns++;
        steps_gap();
        if (g_mode == MODE_LIVE) SerialDBG.println("#OVERRUN");
    }

    uint32_t before = step_counter_steps(&g_steps);
    for (size_t i = 0; i < got; i++) {
        steps_push(s[i]);
        if (g_mode == MODE_FLASH) {
            mlog_status_t ms = mlog_add(&g_log, t, s[i].x, s[i].y, s[i].z);
            if (ms != MLOG_OK) {
                SerialDBG.print("#ERROR log: ");
                SerialDBG.println(mlog_status_str(ms));
                stop_recording();
                return;
            }
            t = mlog_next_time(&g_log);
        } else {
            char line[40];
            snprintf(line, sizeof(line), "%lu,%d,%d,%d", (unsigned long)(g_live_index * SAMPLE_MS), s[i].x, s[i].y,
                     s[i].z);
            SerialDBG.println(line);
            g_live_index++;
        }
    }
    if (g_mode == MODE_LIVE && step_counter_steps(&g_steps) != before) {
        SerialDBG.print("#steps ");
        SerialDBG.println(step_counter_steps(&g_steps));
    }
}

// ---------------------------------------------------------------------------
// Download: one CSV block per session, plus the on-board step count
// ---------------------------------------------------------------------------
static void dump()
{
    if (!g_flash_ok) {
        SerialDBG.println("#dump flash not available");
        return;
    }
    uint32_t pages = mlog_pages_used(&g_log);
    SerialDBG.print("#dump begin pages=");
    SerialDBG.println(pages);
    static mlog_page_t p;
    int cur = -1;
    uint32_t next_t = 0, samples = 0, gaps = 0, bad = 0;
    char line[48];
    for (uint32_t i = 0; i <= pages; i++) {
        IWatchdog.reload();
        mlog_page_kind_t k = (i < pages) ? mlog_read_page(&g_log, i, &p) : MLOG_PAGE_ERASED;
        if (k == MLOG_PAGE_CORRUPT) {
            bad++;
            continue;
        }
        bool end = (k == MLOG_PAGE_ERASED);
        if (cur >= 0 && (end || p.session != cur)) {
            snprintf(line, sizeof(line), "#end session=%d samples=%lu", cur, (unsigned long)samples);
            SerialDBG.print(line);
            snprintf(line, sizeof(line), " gaps=%lu steps=%lu", (unsigned long)gaps,
                     (unsigned long)step_counter_steps(&g_steps));
            SerialDBG.println(line);
            cur = -1;
        }
        if (end) break;
        if (cur < 0) {
            cur = p.session;
            samples = gaps = 0;
            next_t = p.t0_ms;
            steps_reset();
            snprintf(line, sizeof(line), "#rec odr=%u session=%d", p.hz, cur);
            SerialDBG.println(line);
            SerialDBG.println("#t_ms,x_mg,y_mg,z_mg");
        }
        if (p.t0_ms != next_t) {
            gaps++;
            steps_gap();
            snprintf(line, sizeof(line), "#GAP %lu ms", (unsigned long)(p.t0_ms - next_t));
            SerialDBG.println(line);
        }
        for (unsigned j = 0; j < p.n; j++) {
            uint32_t t = p.t0_ms + (j * 1000u + p.hz / 2u) / p.hz;
            snprintf(line, sizeof(line), "%lu,%d,%d,%d", (unsigned long)t, p.s[j][0], p.s[j][1], p.s[j][2]);
            SerialDBG.println(line);
            lis2dh12_sample_t s = { p.s[j][0], p.s[j][1], p.s[j][2] };
            steps_push(s);
            samples++;
        }
        next_t = p.t0_ms + (p.n * 1000u + p.hz / 2u) / p.hz;
    }
    snprintf(line, sizeof(line), "#dump end bad_pages=%lu", (unsigned long)bad);
    SerialDBG.println(line);
}

static void erase_progress(void *, uint32_t done, uint32_t total)
{
    IWatchdog.reload();
    if (done == total || done % 16 == 0) {
        SerialDBG.print("#erase ");
        SerialDBG.print(done);
        SerialDBG.print("/");
        SerialDBG.println(total);
    }
}

static void erase_log(bool full)
{
    if (!g_flash_ok && !full) {
        SerialDBG.println("#erase: flash not available");
        return;
    }
    SerialDBG.println(full ? "#erasing the whole log area (about 1 min) ..." : "#erasing recorded sessions ...");
    mlog_status_t ms = mlog_erase(&g_log, full, erase_progress, nullptr);
    SerialDBG.print("#erase ");
    SerialDBG.println(mlog_status_str(ms));
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
static int g_pending = 0;            // command waiting for 'y'
static uint32_t g_pending_ms = 0;

static void help()
{
    SerialDBG.println("i info | d download | n new session | x stop | e erase (E all) | t check | r live | h help");
}

static void run_command(int k)
{
    switch (k) {
    case 'i': print_info(); break;
    case 'd': stop_recording(); dump(); break;
    case 'n': stop_recording(); start_flash_recording(); break;
    case 'x': stop_recording(); break;
    case 'e': stop_recording(); erase_log(false); break;
    case 'E': stop_recording(); erase_log(true); break;
    case 't':
        stop_recording();
        g_fail = 0;
        SerialDBG.println("=== Full board check (" FW_NAME ") ===");
        check_accel(true);
        check_flash();
        SerialDBG.println(g_fail ? "RESULT: FAILED" : "RESULT: ALL CHECKS PASSED");
        break;
    case 'r': stop_recording(); start_live(); break;
    default: break;
    }
}

static void handle_key(int k)
{
    if (g_pending) {
        int cmd = g_pending;
        g_pending = 0;
        if (k == 'y' || k == 'Y') run_command(cmd);
        else SerialDBG.println("#cancelled");
        return;
    }
    if (g_mode == MODE_LIVE && (k == 's' || k == 'S')) {
        stop_recording();
        return;
    }
    if (g_mode == MODE_LIVE && (k == 'm' || k == 'M')) {
        g_marks++;
        SerialDBG.print("#MARK ");
        SerialDBG.print(g_marks);
        SerialDBG.print(" steps=");
        SerialDBG.println(step_counter_steps(&g_steps));
        return;
    }
    switch (k) {
    case 'i': print_info(); return;
    case 'h': case '?': help(); return;
    case 'd': case 'n': case 'x': case 'e': case 'E': case 't': case 'r':
        // Confirm anything that stops a flash recording or deletes data,
        // so noise on an unplugged RX line cannot do it.
        if (g_mode == MODE_FLASH || k == 'e' || k == 'E') {
            g_pending = k;
            g_pending_ms = millis();
            SerialDBG.print("#send y to confirm '");
            SerialDBG.print((char)k);
            SerialDBG.println("'");
            return;
        }
        run_command(k);
        return;
    default:
        return;
    }
}

void setup()
{
    pinMode(LDO_EN, OUTPUT);
    digitalWrite(LDO_EN, LOW);
    pinMode(ACC_SA0, OUTPUT);
    digitalWrite(ACC_SA0, HIGH);
    // Push-pull outputs of the sensor, configured active low: no pull needed.
    pinMode(ACC_INT1, INPUT);
    pinMode(ACC_INT2, INPUT);

    SerialDBG.begin(115200);
    // Pull-up on USART1 RX (PB7) so an unplugged cable leaves the line idle.
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3u << (7 * 2))) | (1u << (7 * 2));
    delay(300);
    SerialDBG.println();
    SerialDBG.println(FW_NAME);
    print_reset_cause();
    // Watchdog, 25 s: restarts the board if the firmware hangs (a new
    // session starts and the next boot prints "WATCHDOG").
    IWatchdog.begin(25000000UL);
    dev_i2c.begin();
    dev_i2c.setClock(400000);
    attachInterrupt(digitalPinToInterrupt(ACC_INT1), isr_int1, FALLING);
    attachInterrupt(digitalPinToInterrupt(ACC_INT2), isr_int2, FALLING);
    steps_reset();

    g_fail = 0;
    SerialDBG.println();
    SerialDBG.println("=== Stepwhere LIS2DH12 board check (" FW_NAME ") ===");
    check_accel(false);
    check_flash();
    SerialDBG.println(g_fail ? "RESULT: FAILED" : "RESULT: ALL CHECKS PASSED");
    help();
    start_flash_recording();
}

void loop()
{
    IWatchdog.reload();
    while (SerialDBG.available()) handle_key(SerialDBG.read());
    if (g_pending && millis() - g_pending_ms > 5000) {
        g_pending = 0;
        SerialDBG.println("#cancelled (no y)");
    }
    if (g_mode != MODE_IDLE) service_sensor();
    // Status every 30 s while recording, whether or not data arrives.
    if (g_mode == MODE_FLASH && millis() - g_last_status > 30000) {
        g_last_status = millis();
        print_info();
    }
}
