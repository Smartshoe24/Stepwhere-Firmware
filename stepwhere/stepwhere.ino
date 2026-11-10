/**
 * Stepwhere tracker — final firmware
 * STM32G070CB (Rev11) — Neoway N706 LTE + G7A GPS + LIS2DH12 accelerometer
 *
 * Pin Mapping:
 *   GSM TX/RX  : PA2 / PA3   (USART2)
 *   GPS TX/RX  : PB8 / PB9   (USART3)
 *   DEBUG      : PB6 / PB7   (USART1, 115200 baud TTL)
 *   GSM PWRKEY : PB13
 *   LDO_EN     : PA10        (shared GSM + GPS rail)
 *   ACCEL      : I2C PA12 / PA11, SA0 PA1 (driven high -> 0x19)
 *
 * Start-up: rail on (GSM + GPS power together), PWRKEY, accelerometer, then
 * AT -> SIM -> GPS UART -> IMEI -> data. Uploads start once data is up.
 *
 * Location, in priority order:
 *   1. GPS   — a fresh fix (more than MIN_SAT satellites, under 5 s old).
 *   2. Wi-Fi — no fix: an AT+WIFIAPSCAN with at least 2 usable access points.
 *              The serving cell is sent with it, so if the Wi-Fi lookup finds
 *              nothing the server's geolocation falls back to the cell.
 *   3. GSM   — no fix and no usable Wi-Fi: the LTE serving cell (AT+NETDMSG).
 *   Network positions are resolved to lat/long by the server.
 *
 * Steps: LIS2DH12 at 25 Hz into its FIFO, read every 500 ms from TIM17 by
 * accel_steps.cpp and counted by the StepwhereMotion step counter.
 *
 * Libraries required:
 *   - TinyGPSPlus 1.0.3, STM32duino core 2.12.0
 *   - StepwhereMotion, StepwhereOTA (../libraries)
 *
 * AT command reference: Neoway N706 AT Commands Manual V1.0
 */

#include <Wire.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include <stdarg.h>

#include "accel_steps.h"
// OTA (bootloader at 0x08000000, this app at 0x08004000): see ota.ino
#include <W25QArduino.h>
#include <ota_crc32.h>
#include <ota_image.h>
#include <ota_layout.h>
#include <ota_meta.h>
#include <ota_public_key.h>
#include "src/radio_parsers.h"

// Overridable for OTA test builds: -DAPP_VERSION=\"v2.2.1-test\"
#ifndef APP_VERSION
#define APP_VERSION "v2.2.0-lis2dh12"
#endif

// Modem and GPS bytes keep arriving while the background service reads the
// accelerometer; the 64-byte default overflows. Set by build_opt.h.
#if SERIAL_RX_BUFFER_SIZE < 1024
#error "Build with scripts/build-arduino.sh (needs SERIAL_RX_BUFFER_SIZE=1024 from build_opt.h)"
#endif

// ─── Pins ─────────────────────────────────────────────────────────────────────
#define GSM_PWRKEY PB13
#define LDO_EN PA10
#define GSM_RX PA3
#define GSM_TX PA2
#define GPS_RX PB9
#define GPS_TX PB8
#define I2C_SDA PA12
#define I2C_SCL PA11
#define ACCEL_SA0 PA1

#define ADC_PIN PB10       // External analog input (ADC_IN8)
#define ADC_VREF_CH AVREF  // Internal Vref channel (STM32 built-in)
#define BATTERY_ADC_BITS 12  // 12-bit = 0..4095 (the core already defines ADC_RESOLUTION)
#define MIN_BAT_VOLT 3.2f  // Minimum battery voltage 3.20
#define BAT_RATIO 112
// Checked between operations in loop() only: a reading taken while the modem
// powers up or transmits sags under load and would cut a healthy battery.
#define BATTERY_LOW_OFF 3.40f  // below this the modem/GPS rail is cut (v1.2)
#define BATTERY_LOW_ON 3.70f   // and stays cut until the battery recovers (v1.2)

#define CHRG_PIN PB14
#define STDBY_PIN PB15

// ─── Baud rates ───────────────────────────────────────────────────────────────
#define GSM_BAUD 115200
#define GPS_BAUD 9600
#define DBG_BAUD 115200

// ─── Server ───────────────────────────────────────────────────────────────────
#define APN "internet"
#define SERVER_CMD "AT+HTTPPARA=url,\"http://ingest.stepwhere.in/location\""
#define HTTP_PORT 80

// ─── Timing ───────────────────────────────────────────────────────────────────
#define AT_TIMEOUT 3000
#define AT_LONG_TIMEOUT 15000
#define HTTP_TIMEOUT 30000
#define HTTP_REQ_DELAY 8000UL  // gap between the end of one upload and the next

#define MODEM_AT_TIMEOUT_MS 30000UL
#define SIM_READY_TIMEOUT_MS 60000UL
#define SIM_RETRY_DELAY_MS 30000UL
#define MODEM_RAIL_CYCLES 3                // rail cycles before backing off
#define MODEM_RECOVERY_BACKOFF_MS 60000UL
#define DATA_RETRY_DELAY_MS 5000UL
#define DATA_FAILURES_BEFORE_RAIL_CYCLE 6
#define HTTP_TRANSPORT_FAILURES_BEFORE_CHECK 5

// ─── Location ─────────────────────────────────────────────────────────────────
#define MIN_SAT 3                           // a fix needs more satellites than this
#define GPS_FIX_MAX_AGE_MS 5000UL
#define GPS_SILENT_RESTART_MS 120000UL      // no NMEA at all for this long → rail cycle
#define GPS_SILENT_RESTART_HOLDOFF_MS (30UL * 60UL * 1000UL)

#define WIFI_SCAN_INTERVAL_MS 60000UL       // server looks up at most once a minute
#define WIFI_RESULT_MAX_AGE_MS 120000UL
// Scans return ERROR while LTE is busy (attach, data activation, an upload) and
// succeed on a quiet link, so each scan waits this long after data activity.
#define WIFI_SCAN_QUIET_MS 12000UL
#define WIFI_SCAN_RETRY_DELAY_MS 5000UL
#define WIFI_SCAN_TIMEOUT_MS 30000UL
// A timeout power cycles the shared rail (and the GPS); wait longer after one.
#define WIFI_SCAN_TIMEOUT_BACKOFF_MS (10UL * 60UL * 1000UL)
// The server's geolocation only uses Wi-Fi with at least this many usable APs.
#define WIFI_MIN_USABLE_APS 2

// ─── Activity / sleep ─────────────────────────────────────────────────────────
#define MIN_STEPS 20  // steps within a minute that cancel sleep mode
#define IDLE_TIME 5   // awake minutes before sleep is evaluated
#define MINUTE_MS 60000UL
#define WAKE_MIN_DISTANCE_M 15.0

// ─── Logging ──────────────────────────────────────────────────────────────────
#define LVL_NONE 0
#define LVL_ERROR 1
#define LVL_WARN 2
#define LVL_INFO 3
#define LVL_DEBUG 4

#ifndef STEPWHERE_LOG_LEVEL
#define STEPWHERE_LOG_LEVEL LVL_INFO
#endif

HardwareSerial SerialGSM(GSM_RX, GSM_TX);  // USART2 — Neoway N706
HardwareSerial SerialGPS(GPS_RX, GPS_TX);  // USART3 — G7A GPS
HardwareSerial SerialDBG(PB7, PB6);  // USART1 — Debug console

// A disabled level compiles to dead code, so its strings never reach flash.
// (A for-loop rather than if(0) keeps "if (x) INFO.print(); else ..." intact.)
#define LOG_DISABLED for (; false;) SerialDBG

#if STEPWHERE_LOG_LEVEL >= LVL_ERROR
#define ERR SerialDBG
#else
#define ERR LOG_DISABLED
#endif
#if STEPWHERE_LOG_LEVEL >= LVL_WARN
#define WARN SerialDBG
#else
#define WARN LOG_DISABLED
#endif
#if STEPWHERE_LOG_LEVEL >= LVL_INFO
#define INFO SerialDBG
#else
#define INFO LOG_DISABLED
#endif
#if STEPWHERE_LOG_LEVEL >= LVL_DEBUG
#define DEBUG SerialDBG
#else
#define DEBUG LOG_DISABLED
#endif

TwoWire dev_i2c(I2C_SDA, I2C_SCL);
TinyGPSPlus gps;

// =============================================================================
//  STATE
// =============================================================================

enum ChargeStatus : uint8_t {
  CHARGE_UNKNOWN,
  CHARGE_CHARGING,
  CHARGE_COMPLETE,
  CHARGE_ERROR,
  CHARGE_IDLE
};

struct PowerState {
  int battery_level = 0;
  float bat_volt = 0.0f;
  bool battery_low = false;
  ChargeStatus charge = CHARGE_UNKNOWN;
} g_power;

struct ModemState {
  bool rail_on = false;
  uint32_t rail_on_at = 0;
  bool at_ok = false;
  bool functional = false;  // CFUN=1
  bool sim_ready = false;
  bool imei_valid = false;
  bool data_connected = false;
  uint8_t rail_cycles = 0;
  uint8_t data_failures = 0;
  uint8_t transport_failures = 0;
  uint8_t stuck_link_checks = 0;   // transport checks that found a "valid" link
  bool force_data_reactivate = false;
  bool backoff = false;
  uint32_t backoff_started = 0;
  uint32_t backoff_ms = 0;
  bool data_activity_seen = false;
  uint32_t last_data_activity = 0;
  char imei[16] = "";

  bool ready() const {
    return rail_on && at_ok && functional && sim_ready && imei_valid && data_connected;
  }
} g_modem;

struct GpsState {
  bool fix = false;
  bool ever_fixed = false;
  double lat = 0.0;
  double lon = 0.0;
  uint8_t sats = 0;
  uint32_t last_char_at = 0;
  bool uart_on = false;
  bool restart_attempted = false;
  uint32_t restart_at = 0;
} g_gps;

struct StepTracker {
  uint32_t steps_total = 0;
  uint32_t steps_this_minute = 0;
  uint32_t wake_steps = 0;  // steps since the minute boundary, for waking from sleep
  uint16_t minute_counter = 0;
  uint32_t minute_started = 0;
  bool sleep_mode_on = false;
  uint8_t sleep_minute_count = 0;
  double sleep_ref_lat = 0.0;
  double sleep_ref_lon = 0.0;
  bool start_safe_shut_down = false;
  bool send_sleep_data = false;
} g_steps;

// Survives software, watchdog and pin resets (not power loss) so a reset does
// not zero the cumulative step counter the server diffs against.
#define STEP_PERSIST_MAGIC 0x53544550UL
struct PersistedSteps {
  uint32_t magic;
  uint32_t total;
  uint32_t check;
};
PersistedSteps g_persisted_steps __attribute__((section(".noinit")));

// Steps detected in the TIM17 interrupt (accel_steps.cpp), not yet added to
// g_steps. service_motion() moves them over in thread context.
volatile uint32_t g_isr_steps = 0;
bool g_accel_ok = false;

stepwhere::ServingCell g_cell;
stepwhere::WifiScanResult g_wifi;
stepwhere::WifiScanResult g_wifi_scan_buffer;
bool g_wifi_valid = false;
uint32_t g_wifi_scanned_at = 0;
bool g_wifi_attempted = false;
uint32_t g_wifi_last_attempt = 0;
uint32_t g_wifi_scan_interval = WIFI_SCAN_INTERVAL_MS;

enum WifiScanStatus : uint8_t { WIFI_SCAN_OK, WIFI_SCAN_ERROR, WIFI_SCAN_TIMEOUT };
enum HttpResult : uint8_t { HTTP_ACCEPTED, HTTP_REJECTED, HTTP_TRANSPORT_FAILED };
enum LocationSource : uint8_t { LOCATION_GPS, LOCATION_WIFI, LOCATION_GSM, LOCATION_NONE };

struct JsonWriter {
  char *buffer;
  size_t size;
  size_t length;
  bool overflow;

  void raw(const char *text) {
    const size_t add = strlen(text);
    if (overflow || length + add >= size) {
      overflow = true;
      return;
    }
    memcpy(buffer + length, text, add + 1);
    length += add;
  }

  void format(const char *fmt, ...) {
    if (overflow) return;
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer + length, size - length, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= size - length) {
      overflow = true;
      return;
    }
    length += written;
  }

  void coordinate(double value) {
    char text[24];
    dtostrf(value, 0, 8, text);
    raw(text);
  }

  void tenths(int16_t value) {
    const int32_t magnitude = value < 0 ? -(int32_t)value : value;
    format("%s%ld.%ld", value < 0 ? "-" : "", (long)(magnitude / 10), (long)(magnitude % 10));
  }

  void string(const char *text) {
    raw("\"");
    for (const char *c = text; *c; ++c) {
      if (*c == '"' || *c == '\\') {
        const char escaped[3] = { '\\', *c, '\0' };
        raw(escaped);
      } else if ((uint8_t)*c >= 0x20) {
        const char plain[2] = { *c, '\0' };
        raw(plain);
      }
    }
    raw("\"");
  }
};

bool g_upload_attempted = false;
uint32_t g_last_upload = 0;
char g_payload[2560];

// =============================================================================
//  BACKGROUND SERVICE — runs inside every wait so long modem operations never
//  starve the GPS parser, the accelerometer FIFO or the minute bookkeeping.
// =============================================================================

void service_motion();
void service_gps();
void service_minute();
bool gsm_send_at(const char *cmd, const char *expect, uint32_t timeout_ms, String *response = nullptr);

void service_background() {
  static bool running = false;
  if (running) return;
  running = true;
  service_gps();
  service_motion();
  service_minute();
  ota_service();  // feeds the watchdog during an OTA trial boot
  running = false;
}

void wait_ms(uint32_t duration_ms) {
  const uint32_t started = millis();
  while (millis() - started < duration_ms) {
    service_background();
    __WFI();  // SysTick wakes this every millisecond
  }
}

// =============================================================================
//  STEPS
// =============================================================================

uint32_t persisted_check(uint32_t total) {
  return total ^ 0xA5A5A5A5UL ^ STEP_PERSIST_MAGIC;
}

void persist_steps() {
  g_persisted_steps.magic = STEP_PERSIST_MAGIC;
  g_persisted_steps.total = g_steps.steps_total;
  g_persisted_steps.check = persisted_check(g_steps.steps_total);
}

void restore_steps() {
  if (g_persisted_steps.magic == STEP_PERSIST_MAGIC
      && g_persisted_steps.check == persisted_check(g_persisted_steps.total)) {
    g_steps.steps_total = g_persisted_steps.total;
    INFO.print("[STEPS] Restored step counter: ");
    INFO.println(g_steps.steps_total);
  }
  persist_steps();
}

void add_steps(uint32_t count) {
  g_steps.steps_total += count;
  g_steps.steps_this_minute += count;
  g_steps.wake_steps += count;
  persist_steps();
  if (g_steps.sleep_mode_on && g_steps.wake_steps >= MIN_STEPS) {
    g_steps.sleep_mode_on = false;
    g_steps.send_sleep_data = false;
    g_steps.sleep_minute_count = 0;
    g_steps.wake_steps = 0;
    INFO.println("[SLEEP] Walking detected — waking up.");
  }
}

void minute_tick() {
  const uint32_t steps = g_steps.steps_this_minute;
  g_steps.steps_this_minute = 0;
  g_steps.wake_steps = 0;
  g_steps.minute_counter++;
  if (g_steps.minute_counter > 10000) g_steps.minute_counter = 0;

  INFO.print("[MIN] #");
  INFO.print(g_steps.minute_counter);
  INFO.print(" steps=");
  INFO.print(steps);
  INFO.print(" total=");
  INFO.print(g_steps.steps_total);
  INFO.print(" gps=");
  INFO.print(g_gps.fix ? "fix" : "nofix");
  INFO.print(" modem=");
  INFO.print(g_modem.ready() ? "ready" : "down");
  INFO.print(" mode=");
  INFO.print(g_steps.sleep_mode_on ? "sleep" : "active");
  INFO.print(" bat=");
  INFO.println(g_power.bat_volt, 2);
#if STEPWHERE_LOG_LEVEL >= LVL_INFO
  accel_steps_print_status(SerialDBG);
#if STEPS_DEBUG
  accel_steps_print_debug(SerialDBG);
#endif
#endif

  if (g_steps.sleep_mode_on) return;
  if (g_steps.sleep_minute_count < IDLE_TIME) g_steps.sleep_minute_count++;
  if (g_steps.sleep_minute_count < IDLE_TIME) return;

  // Sleep when the last minute had no steps and GPS (if available) shows no
  // movement since the previous evaluation.
  const bool has_gps = g_gps.fix;
  double distance = 0.0;
  if (has_gps) {
    distance = TinyGPSPlus::distanceBetween(g_steps.sleep_ref_lat, g_steps.sleep_ref_lon,
                                            g_gps.lat, g_gps.lon);
  }
  const bool no_movement = !has_gps || distance < WAKE_MIN_DISTANCE_M;
  if (no_movement && steps == 0) {
    INFO.println("[SLEEP] Idle — entering sleep mode.");
    g_steps.sleep_mode_on = true;
    g_steps.send_sleep_data = true;
    g_steps.sleep_minute_count = 0;
    g_steps.start_safe_shut_down = true;
    g_steps.sleep_ref_lat = g_gps.lat;
    g_steps.sleep_ref_lon = g_gps.lon;
  } else if (has_gps) {
    g_steps.sleep_ref_lat = g_gps.lat;
    g_steps.sleep_ref_lon = g_gps.lon;
  }
}

void service_minute() {
  if (millis() - g_steps.minute_started < MINUTE_MS) return;
  g_steps.minute_started += MINUTE_MS;
  minute_tick();
}

// =============================================================================
//  ACCELEROMETER
// =============================================================================

// Called from the TIM17 interrupt by accel_steps.cpp. Only counts: add_steps()
// logs and writes .noinit RAM, so it runs later in service_motion().
void accel_steps_on_steps(uint32_t steps) {
  g_isr_steps += steps;
}

void accel_init() {
  g_accel_ok = accel_steps_begin(dev_i2c, ACCEL_SA0);
  if (g_accel_ok) {
    INFO.println("[ACCEL] LIS2DH12 found at 0x19, step counter running (25 Hz FIFO, TIM17 every 500 ms).");
  } else {
    ERR.println("[ACCEL] LIS2DH12 not responding - retrying every 5 s, no steps until it answers.");
  }
#if STEPWHERE_LOG_LEVEL >= LVL_ERROR
  accel_steps_check_irq_priorities(SerialDBG);
#endif
}

void service_motion() {
  noInterrupts();
  const uint32_t steps = g_isr_steps;
  g_isr_steps = 0;
  interrupts();
  if (steps) add_steps(steps);
}

// =============================================================================
//  GPS
// =============================================================================

// Started only once the SIM is ready: the working wifi-experiment build never
// ran the GPS UART, and the modem must come up exactly as it did there. The
// G7A is powered with the rail regardless, so it acquires satellites meanwhile.
void gps_uart_start() {
  if (g_gps.uart_on || !g_modem.rail_on) return;
  SerialGPS.begin(GPS_BAUD);
  g_gps.uart_on = true;
  g_gps.last_char_at = millis();
  INFO.println("[GPS] UART started.");
}

void service_gps() {
  bool received = false;
  while (g_gps.uart_on && SerialGPS.available()) {
    gps.encode(SerialGPS.read());
    received = true;
  }
  if (received) g_gps.last_char_at = millis();

  const bool fix = gps.location.isValid() && gps.location.age() < GPS_FIX_MAX_AGE_MS
    && gps.satellites.isValid() && gps.satellites.value() > MIN_SAT;
  if (fix) {
    g_gps.lat = gps.location.lat();
    g_gps.lon = gps.location.lng();
    g_gps.sats = gps.satellites.value();
    if (!g_gps.ever_fixed) {
      g_gps.ever_fixed = true;
      g_steps.sleep_ref_lat = g_gps.lat;
      g_steps.sleep_ref_lon = g_gps.lon;
    }
  }
  if (fix != g_gps.fix) {
    INFO.println(fix ? "[GPS] Fix acquired." : "[GPS] Fix lost — using network location.");
  }
  g_gps.fix = fix;
}

// =============================================================================
//  POWER / BATTERY
// =============================================================================

// The modem and GPS UARTs run only while the rail is on. An idle TX line sits
// high and would back-power the unpowered N706/G7A through their inputs, so the
// rail never fully discharges and the modem can ignore the next PWRKEY pulse.
// The GPS UART starts later, from gps_uart_start().
void ldo_enable(bool on) {
  if (!on) {
    SerialGSM.end();
    SerialGPS.end();
    g_gps.uart_on = false;
    pinMode(GSM_TX, INPUT_ANALOG);
    pinMode(GSM_RX, INPUT_ANALOG);
    pinMode(GPS_TX, INPUT_ANALOG);
    pinMode(GPS_RX, INPUT_ANALOG);
  }
  digitalWrite(LDO_EN, on ? HIGH : LOW);
  g_modem.rail_on = on;
  if (on) {
    g_modem.rail_on_at = millis();
    g_gps.last_char_at = millis();
    wait_ms(100);  // let the rail stabilise
    SerialGSM.begin(GSM_BAUD);
  }
}

float adc_read_battery_volts() {
  const uint16_t vref_raw = (uint16_t)analogRead(ADC_VREF_CH);
  if (vref_raw == 0) return 0.0f;
  const float vdda_mv = (1212.0f * 4095.0f) / (float)vref_raw;
  const uint16_t raw = (uint16_t)analogRead(ADC_PIN);
  return (raw / 4095.0f) * vdda_mv / 1000.0f * 2.0f;  // 1:2 divider
}

ChargeStatus read_charge_status() {
  // TP4056 outputs are open-drain and active LOW.
  const bool chrg = digitalRead(CHRG_PIN);
  const bool stdby = digitalRead(STDBY_PIN);
  if (chrg == LOW && stdby == HIGH) return CHARGE_CHARGING;
  if (chrg == HIGH && stdby == LOW) return CHARGE_COMPLETE;
  if (chrg == HIGH && stdby == HIGH) return CHARGE_IDLE;
  return CHARGE_ERROR;
}

const char *charge_status_name(ChargeStatus status) {
  switch (status) {
    case CHARGE_CHARGING: return "charging";
    case CHARGE_COMPLETE: return "charge complete";
    case CHARGE_IDLE: return "not charging";
    case CHARGE_ERROR: return "charger status invalid";
    default: return "unknown";
  }
}

void update_power() {
  const float volts = adc_read_battery_volts();
  if (volts <= 0.0f) return;  // failed Vref read; keep the previous sample
  g_power.bat_volt = volts;
  if (g_power.bat_volt > MIN_BAT_VOLT) {
    int level = (int)((g_power.bat_volt - MIN_BAT_VOLT) * BAT_RATIO);
    g_power.battery_level = level > 100 ? 100 : (level < 0 ? 0 : level);
  } else {
    g_power.battery_level = 0;
  }

  const bool was_low = g_power.battery_low;
  if (g_power.bat_volt < BATTERY_LOW_OFF) g_power.battery_low = true;
  else if (g_power.bat_volt >= BATTERY_LOW_ON) g_power.battery_low = false;
  if (g_power.battery_low && !was_low) {
    WARN.print("[BATTERY] Low — modem and GPS off at ");
    WARN.print(g_power.bat_volt, 2);
    WARN.println(" V");
  } else if (!g_power.battery_low && was_low) {
    INFO.print("[BATTERY] Recovered at ");
    INFO.print(g_power.bat_volt, 2);
    INFO.println(" V");
  }

  const ChargeStatus charge = read_charge_status();
  if (charge != g_power.charge) {
    g_power.charge = charge;
    INFO.print("[BATTERY] ");
    INFO.println(charge_status_name(charge));
  }
}

// =============================================================================
//  AT COMMAND HELPER
// =============================================================================

/**
 * Send an AT command and wait for an expected substring in the response.
 * Pass "" as cmd to only listen (e.g. after sending raw body bytes).
 * Returns false on ERROR or timeout.
 */
bool gsm_send_at(const char *cmd, const char *expect, uint32_t timeout_ms, String *response) {
  if (!g_modem.rail_on) {  // the UART is stopped with the rail
    if (response) *response = "";
    return false;
  }
  while (SerialGSM.available()) SerialGSM.read();
  if (cmd && cmd[0] != '\0') {
    DEBUG.print("[AT] >> ");
    DEBUG.println(cmd);
    SerialGSM.print(cmd);
    SerialGSM.print("\r\n");
  }

  static String reply;
  reply = "";
  reply.reserve(256);
  bool ok = false;
  bool done = false;
  const uint32_t started = millis();

  while (!done && g_modem.rail_on && millis() - started < timeout_ms) {
    while (SerialGSM.available()) {
      reply += (char)SerialGSM.read();
      if (expect && reply.indexOf(expect) != -1) {
        ok = true;
        done = true;
        break;
      }
      if (reply.indexOf("ERROR") != -1) {
        done = true;
        break;
      }
    }
    if (!done) service_background();
  }

  DEBUG.print("[AT] << ");
  DEBUG.println(reply);
  if (response) *response = reply;
  return ok;
}

// Value after `prefix` on a complete reply line, or "" when absent.
String at_response_value(const String &reply, const char *prefix) {
  int offset = 0;
  while (offset < (int)reply.length()) {
    int end = reply.indexOf('\n', offset);
    if (end < 0) break;
    String line = reply.substring(offset, end);
    line.trim();
    if (line.startsWith(prefix)) {
      line.remove(0, strlen(prefix));
      line.trim();
      return line;
    }
    offset = end + 1;
  }
  return "";
}

// =============================================================================
//  MODEM POWER AND READINESS
// =============================================================================

void mark_data_activity() {
  g_modem.data_activity_seen = true;
  g_modem.last_data_activity = millis();
}

void modem_invalidate() {
  g_modem.at_ok = false;
  g_modem.functional = false;
  g_modem.sim_ready = false;
  g_modem.data_connected = false;
}

/** Rail on, then PWRKEY held LOW for ≥ 1 s (N706 power-on sequence). */
void modem_power_on() {
  INFO.println("[MODEM] Power on.");
  ldo_enable(true);
  wait_ms(500);
  digitalWrite(GSM_PWRKEY, HIGH);
  wait_ms(100);
  digitalWrite(GSM_PWRKEY, LOW);
  wait_ms(1200);
  digitalWrite(GSM_PWRKEY, HIGH);
  wait_ms(3000);
  while (SerialGSM.available()) SerialGSM.read();  // flush boot noise
}

/** Removes and restores the shared GSM/GPS rail. */
void modem_rail_cycle(const char *reason) {
  WARN.print("[MODEM] Power cycling: ");
  WARN.println(reason);
  modem_invalidate();
  ldo_enable(false);
  wait_ms(2000);
  modem_power_on();
}

/** Radio and SIM off (AT+CFUN=0) for sleep; the rail stays on for GPS. */
void modem_radio_off() {
  gsm_send_at("AT+CFUN=0", "OK", 5000);
  wait_ms(1000);
  modem_invalidate();
}

void modem_backoff(uint32_t duration_ms) {
  g_modem.backoff = true;
  g_modem.backoff_started = millis();
  g_modem.backoff_ms = duration_ms;
}

bool modem_wait_for_at() {
  const uint32_t started = millis();
  while (g_modem.rail_on && millis() - started < MODEM_AT_TIMEOUT_MS) {
    if (gsm_send_at("AT", "OK", AT_TIMEOUT)) return true;
    wait_ms(1000);
  }
  return false;
}

bool modem_restore_functionality() {
  String reply;
  if (!gsm_send_at("AT+CFUN?", "OK", AT_TIMEOUT, &reply)) return false;
  const String mode = at_response_value(reply, "+CFUN:");
  if (mode == "1") return true;
  if (mode != "0" && mode != "4") {
    WARN.print("[MODEM] Unexpected CFUN state: ");
    WARN.println(mode);
    return false;
  }
  if (!gsm_send_at("AT+CFUN=1", "OK", AT_LONG_TIMEOUT)) return false;
  return gsm_send_at("AT+CFUN?", "OK", AT_TIMEOUT, &reply) && at_response_value(reply, "+CFUN:") == "1";
}

// `last_reply` keeps the final AT+CPIN? answer so a failure can be reported.
bool modem_wait_for_sim(String &last_reply) {
  const uint32_t started = millis();
  while (g_modem.rail_on && millis() - started < SIM_READY_TIMEOUT_MS) {
    if (gsm_send_at("AT+CPIN?", "OK", AT_TIMEOUT, &last_reply)
        && at_response_value(last_reply, "+CPIN:") == "READY") {
      return true;
    }
    wait_ms(2000);
  }
  return false;
}

bool modem_read_imei() {
  String reply;
  if (!gsm_send_at("AT+CGSN", "OK", AT_TIMEOUT, &reply)) return false;
  // Reply: [+CGSN: ]<15 digits> ... OK
  const int echo = reply.indexOf("+CGSN: ");
  if (echo != -1) reply = reply.substring(echo + 7);
  const int ok = reply.lastIndexOf("OK");
  if (ok != -1) reply = reply.substring(0, ok);
  reply.trim();
  if (reply.length() != 15) return false;
  for (uint8_t i = 0; i < 15; ++i) {
    if (!isdigit((unsigned char)reply.charAt(i))) return false;
  }
  reply.toCharArray(g_modem.imei, sizeof(g_modem.imei));
  return true;
}

// "+XIIC: <state>,<ip>" — connected only with state 1 and a nonzero IPv4 address.
bool parse_xiic_connected(const String &value, char *ip_out, size_t ip_size) {
  int state = 0;
  char ip[40] = "";
  char extra = '\0';
  if (sscanf(value.c_str(), " %d , %39s %c", &state, ip, &extra) != 2 || state != 1) return false;
  const char *cursor = ip;
  bool nonzero = false;
  for (uint8_t octet = 0; octet < 4; ++octet) {
    if (!isdigit((unsigned char)*cursor)) return false;
    char *end = nullptr;
    const long part = strtol(cursor, &end, 10);
    if (part < 0 || part > 255) return false;
    nonzero |= part != 0;
    if ((octet < 3 && *end != '.') || (octet == 3 && *end != '\0')) return false;
    cursor = end + (octet < 3 ? 1 : 0);
  }
  if (!nonzero) return false;
  snprintf(ip_out, ip_size, "%s", ip);
  return true;
}

bool modem_data_verified(char *ip, size_t ip_size) {
  String reply;
  if (!gsm_send_at("AT+XIIC?", "OK", 5000, &reply)) return false;
  return parse_xiic_connected(at_response_value(reply, "+XIIC:"), ip, ip_size);
}

/** AT+CGDCONT → AT+XIIC=1 → verified IP (N706 AT Manual §5.2). */
bool modem_data_connect() {
  char ip[40];
  const bool force = g_modem.force_data_reactivate;
  g_modem.force_data_reactivate = false;
  if (!force && modem_data_verified(ip, sizeof(ip))) {
    INFO.print("[MODEM] Data connected, IP ");
    INFO.println(ip);
    return true;
  }
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) return false;
  gsm_send_at("AT+XIIC=0", "OK", AT_TIMEOUT);  // close any stale link
  wait_ms(500);
  const bool activated = gsm_send_at("AT+XIIC=1", "OK", AT_LONG_TIMEOUT);
  mark_data_activity();
  if (!activated) return false;
  wait_ms(1000);  // let IP assignment settle
  if (!modem_data_verified(ip, sizeof(ip))) return false;
  INFO.print("[MODEM] Data connected, IP ");
  INFO.println(ip);
  return true;
}

/** Brings the modem one step closer to ready; true once uploads can run. */
bool modem_ensure_ready() {
  if (g_modem.backoff) {
    if (millis() - g_modem.backoff_started < g_modem.backoff_ms) return false;
    g_modem.backoff = false;
  }
  if (g_power.battery_low && !g_steps.send_sleep_data) return false;
  if (!g_modem.rail_on) modem_power_on();
  if (!g_modem.rail_on) return false;

  if (!g_modem.at_ok) {
    if (!modem_wait_for_at()) {
      if (!g_modem.rail_on) return false;  // cut for low battery meanwhile
      if (g_modem.rail_cycles < MODEM_RAIL_CYCLES) {
        g_modem.rail_cycles++;
        modem_rail_cycle("no AT response");
      } else {
        ERR.println("[MODEM] Not responding after power cycles — retrying in 60 s.");
        g_modem.rail_cycles = 0;
        modem_backoff(MODEM_RECOVERY_BACKOFF_MS);
      }
      return false;
    }
    g_modem.at_ok = true;
    g_modem.rail_cycles = 0;
    gsm_send_at("ATE0", "OK", AT_TIMEOUT);
    gsm_send_at("AT+CMEE=2", "OK", AT_TIMEOUT);  // verbose +CME ERROR text
  }

  if (!g_modem.functional) {
    g_modem.functional = modem_restore_functionality();
    if (!g_modem.functional) {
      WARN.println("[MODEM] Could not enable radio (CFUN=1).");
      modem_backoff(DATA_RETRY_DELAY_MS);
      return false;
    }
  }

  if (!g_modem.sim_ready) {
    String reply;
    g_modem.sim_ready = modem_wait_for_sim(reply);
    if (!g_modem.sim_ready) {
      reply.replace("\r\n", " ");
      reply.trim();
      WARN.print("[MODEM] SIM not ready (");
      WARN.print(reply.length() ? reply : String("no reply"));
      WARN.println(") — retrying in 30 s.");
      modem_backoff(SIM_RETRY_DELAY_MS);
      return false;
    }
    INFO.println("[MODEM] SIM ready.");
    gps_uart_start();
  }

  if (!g_modem.imei_valid) {
    g_modem.imei_valid = modem_read_imei();
    if (!g_modem.imei_valid) {
      WARN.println("[MODEM] IMEI read failed.");
      modem_backoff(3000);
      return false;
    }
    INFO.print("[MODEM] IMEI ");
    INFO.println(g_modem.imei);
  }

  if (!g_modem.data_connected) {
    g_modem.data_connected = modem_data_connect();
    if (!g_modem.data_connected) {
      WARN.println("[MODEM] Data activation failed.");
      if (++g_modem.data_failures >= DATA_FAILURES_BEFORE_RAIL_CYCLE) {
        g_modem.data_failures = 0;
        modem_rail_cycle("data activation keeps failing");
      } else {
        modem_backoff(DATA_RETRY_DELAY_MS);
      }
      return false;
    }
    g_modem.data_failures = 0;
    g_modem.transport_failures = 0;
  }
  return true;
}

/** After repeated transport failures, re-verify instead of blindly reconnecting. */
void modem_check_transport() {
  if (++g_modem.transport_failures < HTTP_TRANSPORT_FAILURES_BEFORE_CHECK) return;
  g_modem.transport_failures = 0;
  if (!gsm_send_at("AT", "OK", AT_TIMEOUT)) {
    WARN.println("[MODEM] Unresponsive after upload failures.");
    modem_invalidate();
    return;
  }
  char ip[40];
  if (!modem_data_verified(ip, sizeof(ip))) {
    WARN.println("[MODEM] Data link lost — reconnecting.");
    g_modem.data_connected = false;
    return;
  }
  // The modem reports a connected IP yet uploads keep failing.
  if (++g_modem.stuck_link_checks >= 2) {
    g_modem.stuck_link_checks = 0;
    modem_rail_cycle("uploads keep failing on a connected link");
  } else {
    WARN.println("[MODEM] Uploads failing on a connected link — re-activating data.");
    g_modem.force_data_reactivate = true;
    g_modem.data_connected = false;
  }
}

// =============================================================================
//  NETWORK LOCATION OBSERVATIONS
// =============================================================================

void query_serving_cell() {
  String response;
  const char *error = nullptr;
  if (!gsm_send_at("AT+NETDMSG", "OK", AT_TIMEOUT, &response)) {
    g_cell.valid = false;
    WARN.println("[CELL] AT+NETDMSG failed.");
    return;
  }
  if (!stepwhere::parse_netdmsg(response.c_str(), g_cell, error)) {
    WARN.print("[CELL] Serving cell rejected: ");
    WARN.println(error);
    return;
  }
  INFO.print("[CELL] mcc=");
  INFO.print(g_cell.mobile_country_code);
  INFO.print(" mnc=");
  INFO.print(g_cell.mobile_network_code);
  INFO.print(" tac=");
  INFO.print(g_cell.tracking_area_code);
  INFO.print(" cellId=");
  INFO.println(g_cell.cell_id);
}

WifiScanStatus wifi_scan(stepwhere::WifiScanResult &result) {
  result.clear();
  if (!g_modem.rail_on) return WIFI_SCAN_ERROR;
  while (SerialGSM.available()) SerialGSM.read();
  DEBUG.println("[AT] >> AT+WIFIAPSCAN");
  SerialGSM.print("AT+WIFIAPSCAN\r\n");

  const uint32_t started = millis();
  char line[192];
  uint16_t length = 0;
  bool overflow = false;
  while (millis() - started < WIFI_SCAN_TIMEOUT_MS) {
    if (!g_modem.rail_on) {
      result.clear();
      return WIFI_SCAN_ERROR;
    }
    while (SerialGSM.available()) {
      const char c = (char)SerialGSM.read();
      if (c == '\r') continue;
      if (c != '\n') {
        if (length < sizeof(line) - 1) line[length++] = c;
        else overflow = true;
        continue;
      }
      line[length] = '\0';
      if (!overflow) {
        DEBUG.print("[AT] << ");
        DEBUG.println(line);
        if (strcmp(line, "OK") == 0) return WIFI_SCAN_OK;
        if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR:", 11) == 0) {
          result.clear();
          return WIFI_SCAN_ERROR;
        }
        if (strncmp(line, "+WIFIAPSCAN:", 12) == 0 && !stepwhere::parse_wifi_ap_line(line, result, millis())) {
          DEBUG.println("[WIFI] Ignored malformed access point line.");
        }
      }
      length = 0;
      overflow = false;
    }
    service_background();
  }
  result.clear();
  return WIFI_SCAN_TIMEOUT;
}

/**
 * Refreshes g_wifi at most once per WIFI_SCAN_INTERVAL_MS, after the radio has
 * been quiet. Returns false when a scan timeout forced a modem power cycle.
 */
bool refresh_wifi_observation() {
  if (g_wifi_attempted && millis() - g_wifi_last_attempt < g_wifi_scan_interval) return true;
  g_wifi_attempted = true;

  WifiScanStatus status = WIFI_SCAN_ERROR;
  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    const uint32_t quiet_ms = attempt == 1 ? WIFI_SCAN_QUIET_MS : WIFI_SCAN_RETRY_DELAY_MS;
    if (g_modem.data_activity_seen) {
      const uint32_t since = millis() - g_modem.last_data_activity;
      if (since < quiet_ms) wait_ms(quiet_ms - since);
    }
    const uint32_t started = millis();
    status = wifi_scan(g_wifi_scan_buffer);
    if (status == WIFI_SCAN_TIMEOUT) {
      g_wifi_last_attempt = millis();
      g_wifi_scan_interval = WIFI_SCAN_TIMEOUT_BACKOFF_MS;
      modem_rail_cycle("Wi-Fi scan timed out; next scan in 10 minutes");
      return false;
    }
    if (status == WIFI_SCAN_OK) {
      INFO.print("[WIFI] Scan found ");
      INFO.print(g_wifi_scan_buffer.count);
      INFO.print(" access points in ");
      INFO.print(millis() - started);
      INFO.println(" ms.");
      break;
    }
    WARN.print("[WIFI] Scan returned ERROR (attempt ");
    WARN.print(attempt);
    WARN.println("/2).");
    // A retry is measured from this failure, not from the last upload.
    mark_data_activity();
  }
  g_wifi_last_attempt = millis();

  if (status == WIFI_SCAN_OK) {
    g_wifi_scan_interval = WIFI_SCAN_INTERVAL_MS;
    g_wifi = g_wifi_scan_buffer;
    g_wifi_valid = g_wifi.count > 0;
    g_wifi_scanned_at = millis();
  }
  // After an ERROR the previous scan stays usable until it is too old.
  return true;
}

// Access points the server's geolocation (Google) accepts: globally
// administered unicast BSSIDs, not VRRP, with RSSI at or below -10 dBm.
uint8_t wifi_usable_ap_count() {
  uint8_t usable = 0;
  for (uint8_t i = 0; i < g_wifi.count; ++i) {
    const stepwhere::WifiAccessPoint &point = g_wifi.points[i];
    const uint8_t first_octet = (uint8_t)strtoul(point.mac, nullptr, 16);
    if ((first_octet & 0x03) != 0) continue;
    if (strncmp(point.mac, "00:00:5E:", 9) == 0 || strcmp(point.mac, "00:00:00:00:00:00") == 0) continue;
    if (point.rssi > -10) continue;
    ++usable;
  }
  return usable;
}

bool wifi_observation_usable() {
  return g_wifi_valid && millis() - g_wifi_scanned_at <= WIFI_RESULT_MAX_AGE_MS
    && wifi_usable_ap_count() >= WIFI_MIN_USABLE_APS;
}

const char *location_source_name(LocationSource source) {
  switch (source) {
    case LOCATION_GPS: return "gps";
    case LOCATION_WIFI: return "wifi";
    case LOCATION_GSM: return "gsm";
    default: return "none";
  }
}

// =============================================================================
//  TELEMETRY PAYLOAD
// =============================================================================

void append_cellular(JsonWriter &json) {
  const stepwhere::ServingCell &cell = g_cell;
  json.format("\"cellular\":{\"radioType\":\"lte\",\"duplexMode\":\"%s\",\"towers\":[{\"role\":\"serving\"",
              cell.duplex_mode);
  json.format(",\"mobileCountryCode\":%u,\"mobileNetworkCode\":%u,\"trackingAreaCode\":%u,\"cellId\":%lu",
              cell.mobile_country_code, cell.mobile_network_code, cell.tracking_area_code,
              (unsigned long)cell.cell_id);
  if (cell.has_physical_cell_id) json.format(",\"physicalCellId\":%u", cell.physical_cell_id);
  if (cell.has_band) {
    json.raw(",\"band\":");
    json.string(cell.band);
  }
  if (cell.has_earfcn) json.format(",\"earfcn\":%lu", (unsigned long)cell.earfcn);
  if (cell.has_rx_dbm) json.format(",\"rxDbm\":%d", cell.rx_dbm);
  if (cell.has_tx_dbm) json.format(",\"txDbm\":%d", cell.tx_dbm);
  if (cell.has_rsrp) {
    json.raw(",\"rsrpDbm\":");
    json.tenths(cell.rsrp_tenths_dbm);
  }
  if (cell.has_rsrq) {
    json.raw(",\"rsrqDb\":");
    json.tenths(cell.rsrq_tenths_db);
  }
  if (cell.has_sinr) {
    json.raw(",\"sinrDb\":");
    json.tenths(cell.sinr_tenths_db);
  }
  json.raw("}]},");
}

void append_wifi(JsonWriter &json) {
  const uint32_t now = millis();
  json.raw("\"wifi\":{\"accessPoints\":[");
  for (uint8_t i = 0; i < g_wifi.count; ++i) {
    const stepwhere::WifiAccessPoint &point = g_wifi.points[i];
    json.format("%s{\"macAddress\":\"%s\",\"signalStrengthDbm\":%d,\"channel\":%u,\"ageMs\":%lu}",
                i ? "," : "", point.mac, point.rssi, point.channel,
                (unsigned long)(now - point.observed_at_ms));
  }
  json.raw("]},");
}

/** Builds the ingest JSON into g_payload; returns its length, or 0 on overflow. */
size_t build_payload(bool gps_fix, bool include_cell, bool include_wifi) {
  JsonWriter json = { g_payload, sizeof(g_payload), 0, false };
  g_payload[0] = '\0';

  int battery_mv = 1;
  if (g_power.bat_volt > 0.0f) {
    battery_mv = (int)(g_power.bat_volt * 1000.0f + 0.5f);
    if (battery_mv < 1) battery_mv = 1;
  }

  json.raw("{\"hardwareIdentifier\":");
  json.string(g_modem.imei);
  json.raw(",\"latitude\":");
  json.coordinate(gps_fix ? g_gps.lat : 0.0);
  json.raw(",\"longitude\":");
  json.coordinate(gps_fix ? g_gps.lon : 0.0);
  json.raw(gps_fix ? ",\"gpsFixValid\":true," : ",\"gpsFixValid\":false,");
  if (include_cell) append_cellular(json);
  if (include_wifi) append_wifi(json);
  json.format("\"accuracyMeters\":null,\"batteryPercent\":%d,\"batteryVoltageMv\":%d,\"signalStrength\":0",
              g_power.battery_level, battery_mv);
  json.format(",\"satelliteCount\":%u,\"stepCounter\":%lu,\"deviceState\":\"%s\"",
              gps_fix ? g_gps.sats : 0, (unsigned long)g_steps.steps_total,
              g_steps.sleep_mode_on ? "sleep" : "active");
  // Firmware/OTA status; skipped rather than overflowing a large Wi-Fi payload.
  if (json.length + 220 < sizeof(g_payload)) ota_append_status(json);
  json.raw("}");
  return json.overflow ? 0 : json.length;
}

// =============================================================================
//  HTTP UPLOAD
// =============================================================================

// Status of the last "HTTP/1.x NNN" line (skipping interim 1xx responses), or
// 201 for the bare " 201 " v1.2 accepted; 0 when no status was received.
int http_status_code(const String &response) {
  int code = 0;
  for (int index = response.indexOf("HTTP/1."); index >= 0; index = response.indexOf("HTTP/1.", index + 1)) {
    const int space = response.indexOf(' ', index);
    if (space < 0) break;
    const int parsed = response.substring(space + 1, space + 4).toInt();
    if (parsed >= 200 && parsed <= 599) code = parsed;
  }
  if (code == 0 && response.indexOf(" 201 ") != -1) code = 201;
  return code;
}

HttpResult http_post_json(const char *payload, size_t length, int &status_code) {
  status_code = 0;
  char cmd[48];
  HttpResult result = HTTP_TRANSPORT_FAILED;

  if (!gsm_send_at(SERVER_CMD, "OK", AT_TIMEOUT)) {
    mark_data_activity();
    return result;
  }
  snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=port,%d", HTTP_PORT);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    mark_data_activity();
    return result;
  }
  if (!gsm_send_at("AT+HTTPSETUP", "OK", AT_LONG_TIMEOUT)) {
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    mark_data_activity();
    return result;
  }
  snprintf(cmd, sizeof(cmd), "AT+HTTPACTION=2,%u,2", (unsigned)length);
  if (!gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    mark_data_activity();
    return result;
  }

  SerialGSM.print(payload);

  String response;
  if (gsm_send_at("", "+HTTPRECV:", HTTP_TIMEOUT, &response)) {
    const uint32_t started = millis();
    while (millis() - started < 5000) {
      while (SerialGSM.available()) response += (char)SerialGSM.read();
      if (response.indexOf("+HTTPCLOSED:") != -1) break;
      service_background();
    }
    status_code = http_status_code(response);
    if (status_code == 201) result = HTTP_ACCEPTED;
    else if (status_code != 0) result = HTTP_REJECTED;
    DEBUG.print("[HTTP] Response: ");
    DEBUG.println(response);
  }
  gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
  mark_data_activity();
  return result;
}

void upload_cycle() {
  const bool sleep_upload = g_steps.sleep_mode_on && g_steps.send_sleep_data;

  // 1. GPS: a fresh fix needs no network observations.
  // 2. Wi-Fi: no fix, so scan (rate-limited) and use it with >= 2 usable APs.
  //    The serving cell is attached too: when the Wi-Fi lookup finds nothing
  //    the server's geolocation falls back to the cell in the same request.
  // 3. GSM: no fix and no usable Wi-Fi, so the serving cell alone.
  g_cell.valid = false;
  if (!g_gps.fix) {
    if (!refresh_wifi_observation()) return;  // the modem is being recovered
    query_serving_cell();
  }
  // Re-read after the scan: a fix may have arrived during the wait.
  const bool gps_fix = g_gps.fix;
  const bool include_wifi = !gps_fix && wifi_observation_usable();
  const bool include_cell = !gps_fix && g_cell.valid;
  const LocationSource source = gps_fix ? LOCATION_GPS
    : include_wifi ? LOCATION_WIFI
    : include_cell ? LOCATION_GSM
    : LOCATION_NONE;

  const size_t length = build_payload(gps_fix, include_cell, include_wifi);
  if (length == 0) {
    ERR.println("[HTTP] Payload did not fit its buffer; upload skipped.");
    return;
  }

  INFO.print("[UPLOAD] source=");
  INFO.print(location_source_name(source));
  if (gps_fix) {
    INFO.print(" ");
    INFO.print(g_gps.lat, 6);
    INFO.print(",");
    INFO.print(g_gps.lon, 6);
    INFO.print(" sats=");
    INFO.print(g_gps.sats);
  } else {
    INFO.print(" wifiAps=");
    INFO.print(include_wifi ? g_wifi.count : 0);
    INFO.print(" cell=");
    INFO.print(include_cell ? "yes" : "no");
  }
  INFO.print(" steps=");
  INFO.print(g_steps.steps_total);
  INFO.print(" state=");
  INFO.print(g_steps.sleep_mode_on ? "sleep" : "active");
  INFO.print(" bytes=");
  INFO.println(length);
  DEBUG.println(g_payload);

  const uint32_t started = millis();
  int status_code = 0;
  HttpResult result = http_post_json(g_payload, length, status_code);
  if (result == HTTP_REJECTED && (status_code == 400 || status_code == 422) && (include_cell || include_wifi)) {
    WARN.print("[HTTP] Server returned ");
    WARN.print(status_code);
    WARN.println(" — resending without network observations.");
    const size_t minimal = build_payload(gps_fix, false, false);
    if (minimal) result = http_post_json(g_payload, minimal, status_code);
  }
  const uint32_t elapsed = millis() - started;

  if (result == HTTP_TRANSPORT_FAILED) {
    WARN.print("[HTTP] Upload failed (no server response) after ");
    WARN.print(elapsed);
    WARN.println(" ms.");
    modem_check_transport();
    return;
  }
  g_modem.transport_failures = 0;
  g_modem.stuck_link_checks = 0;
  if (result == HTTP_REJECTED) {
    WARN.print("[HTTP] Server returned ");
    WARN.println(status_code);
    return;
  }
  ota_on_upload_accepted();
  INFO.print("[HTTP] 201 in ");
  INFO.print(elapsed);
  INFO.println(" ms.");
  if (sleep_upload && g_steps.sleep_mode_on) g_steps.send_sleep_data = false;
}

// =============================================================================
//  GPS RECOVERY
// =============================================================================

/** A G7A that has stopped sending NMEA entirely is restarted with the rail. */
void check_gps_alive() {
  if (!g_modem.rail_on || !g_gps.uart_on) return;
  if (millis() - g_gps.last_char_at < GPS_SILENT_RESTART_MS) return;
  if (g_gps.restart_attempted && millis() - g_gps.restart_at < GPS_SILENT_RESTART_HOLDOFF_MS) return;
  g_gps.restart_attempted = true;
  g_gps.restart_at = millis();
  modem_rail_cycle("GPS sent no data for 2 minutes");
}

// =============================================================================
//  SETUP
// =============================================================================

void log_reset_cause(uint32_t flags) {
  INFO.print("[BOOT] Reset cause:");
  if (flags & RCC_CSR_PWRRSTF) INFO.print(" power-on/brownout");
  if (flags & RCC_CSR_PINRSTF) INFO.print(" reset-pin");
  if (flags & RCC_CSR_SFTRSTF) INFO.print(" software");
  if (flags & RCC_CSR_IWDGRSTF) INFO.print(" independent-watchdog");
  if (flags & RCC_CSR_WWDGRSTF) INFO.print(" window-watchdog");
  INFO.println();
}

void setup() {
  const uint32_t reset_flags = RCC->CSR;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  pinMode(GSM_PWRKEY, OUTPUT);
  digitalWrite(GSM_PWRKEY, HIGH);  // PWRKEY idle high
  pinMode(LDO_EN, OUTPUT);
  digitalWrite(LDO_EN, LOW);       // rail off until the modem sequence

  SerialDBG.begin(DBG_BAUD);
  delay(50);
  INFO.println("[BOOT] Stepwhere tracker " APP_VERSION);
  log_reset_cause(reset_flags);
  ota_begin();

  analogReadResolution(BATTERY_ADC_BITS);
  pinMode(ADC_PIN, INPUT_ANALOG);
  pinMode(CHRG_PIN, INPUT_PULLUP);
  pinMode(STDBY_PIN, INPUT_PULLUP);
  update_power();
  INFO.print("[BOOT] Battery ");
  INFO.print(g_power.bat_volt, 2);
  INFO.println(" V");

  restore_steps();
  g_steps.minute_started = millis();

  wait_ms(2000);  // let the shared rail discharge before a fresh PWRKEY pulse
  // Powered whatever the battery reads, as in v1.2; loop() cuts it if low.
  modem_power_on();
  // Accelerometer after the PWRKEY sequence, as in the wifi-experiment build.
  accel_init();
  modem_ensure_ready();
  INFO.println("[BOOT] Init complete.");
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
  service_background();
  update_power();

  if (g_power.battery_low && !g_steps.send_sleep_data) {
    if (g_modem.rail_on) {
      ldo_enable(false);
      modem_invalidate();
    }
    wait_ms(5000);
    return;
  }

  if (g_steps.sleep_mode_on && !g_steps.send_sleep_data) {
    if (g_steps.start_safe_shut_down) {
      INFO.println("[SLEEP] Modem radio off.");
      modem_radio_off();
      g_steps.start_safe_shut_down = false;
    }
    wait_ms(1000);
    return;
  }

  check_gps_alive();
  if (!modem_ensure_ready()) {
    wait_ms(1000);
    return;
  }

  if (!g_upload_attempted || millis() - g_last_upload >= HTTP_REQ_DELAY) {
    upload_cycle();
    // The interval starts after the upload, which also gives the radio quiet
    // time before the next Wi-Fi scan.
    g_last_upload = millis();
    g_upload_attempted = true;
  } else {
    ota_tick();  // daily update check / one download chunk between uploads
  }
  wait_ms(200);
}
