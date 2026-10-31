/**
 * STM32G071CB — Neoway N706 GSM + G7A GPS
 *
 * Pin Mapping:
 *   GSM TX/RX  : PA2 / PA3   (USART2)
 *   GPS TX/RX  : PB8 / PB9   (USART3)
 *   DEBUG      : PB6 / PB7   (USART1)
 *   GSM PWRKEY : PB13
 *   LDO_EN     : PA10
 *
 * Libraries required:
 *   - TinyGPSPlus  (by Mikal Hart)
 *   - STM32duino core
 *
 * AT command reference: Neoway N706 AT Commands Manual V1.0
 */

#include <STM32TimerInterrupt.h>  //v1.3.0
#include <Wire.h>
#include <STM32_ISR_Timer.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include "accel_steps.h"  // LIS2DH12 + software step counter (libraries/StepwhereMotion)
// #include "cert.h"

// ─── Pin definitions ──────────────────────────────────────────────────────────
#define GSM_PWRKEY PB13
#define LDO_EN PA10
#define DEBUG_EN
#define I2C_SDA PA12
#define I2C_SCL PA11
#define ACC_SA0 PA1    // LIS2DH12 SA0, driven HIGH -> I2C address 0x19
#define ACC_INT1 PB3   // LIS2DH12 INT1 (not used yet)
#define ACC_INT2 PB4   // LIS2DH12 INT2 (not used yet, for wake from sleep)

#define ADC_PIN PB10       // External analog input (ADC_IN8)
#define ADC_VREF_CH AVREF  // Internal Vref channel (STM32 built-in)
#define ADC_RESOLUTION 12  // 12-bit = 0..4095
#define VREF_MV        3300.0f    // Supply / Vref voltage in mV
#define MIN_BAT_VOLT   3.2f  // Minimum battery voltage 3.20
#define BAT_RATIO      112      //100      //230
/*
* 143 == (100 /(4.2 - 3.5))
* 112 == (100 / (4.1 - 3.2))
*/

#define BATTERY_LOW_OFF 3.40f
#define BATTERY_LOW_ON 3.70f

#define BMS_STATUS1 PB15  //No  battery charging status on controller
#define BMS_STATUS2 PB14  //No  battery charging status on controller

#define CHRG_PIN PB14
#define STDBY_PIN PB15

typedef enum ChargeStatus {
  CHARGE_UNKNOWN,
  CHARGE_CHARGING,
  CHARGE_COMPLETE,
  CHARGE_ERROR,
  CHARGE_IDLE
};

struct vdd_mon {
  int battery_level;
  float bat_volt;
  float vdd_volt;
} g_vdd_mon;


#define GPS_RESET_MAGIC 0xCAFE1234UL

uint32_t gpsreset_magic
  __attribute__((section(".noinit")));

// ─── Serial port aliases ──────────────────────────────────────────────────────
// STM32duino HardwareSerial constructor takes (RX_pin, TX_pin).

HardwareSerial SerialGSM(PA3, PA2);  // USART2 — Neoway N706
HardwareSerial SerialGPS(PB9, PB8);  // USART3 — G7A GPS
HardwareSerial SerialDBG(PB7, PB6);  // USART1 — Debug console

TwoWire dev_i2c(I2C_SDA, I2C_SCL);

STM32Timer ITimer(TIM1);    // Init STM32 timer TIM1
STM32_ISR_Timer ISR_Timer;  // Init STM32_ISR_Timer

// #define INFO SerialDBG

// #ifdef DEBUG_EN
// #define DEBUG SerialDBG
// #else
// #define DEBUG
// #endif


#define LOG_SERIAL SerialDBG   // e.g. #define SerialDBG Serial

// ---------------------------------------------------------------
// Level values (higher = more verbose)
// ---------------------------------------------------------------
#define LVL_NONE    0
#define LVL_ERROR   1
#define LVL_WARN    2
#define LVL_INFO    3
#define LVL_DEBUG   4
#define LVL_VERBOSE 5

#define LOG_LEVEL LVL_DEBUG

// Set active level (override with -D LOG_LEVEL=x in build flags)
#ifndef LOG_LEVEL
#define LOG_LEVEL LVL_INFO
#endif

class NullStream : public Print {
public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *buffer, size_t size) override { return size; }
};

NullStream nullStream;   // define once in a .cpp, see below


#if LOG_LEVEL >= LVL_ERROR
  #define ERR   LOG_SERIAL
#else
  #define ERR   nullStream
#endif

#if LOG_LEVEL >= LVL_WARN
  #define WARN  LOG_SERIAL
#else
  #define WARN  nullStream
#endif

#if LOG_LEVEL >= LVL_INFO
  #define INFO  LOG_SERIAL
#else
  #define INFO  
#endif

#if LOG_LEVEL >= LVL_DEBUG
  #define DEBUG LOG_SERIAL
#else
  #define DEBUG nullStream
#endif



#define MIN_SAT 3
#define GPS_NO_FIX_TIMEOUT_MS (10UL * 60UL * 1000UL)

uint32_t gpsNoFixStart = 0;
bool gpsNoFixTimerRunning = false;


// ─── Baud rates ───────────────────────────────────────────────────────────────
#define GSM_BAUD 115200
#define GPS_BAUD 9600
#define DBG_BAUD 115200

// ─── Timeouts (ms) ───────────────────────────────────────────────────────────
#define AT_TIMEOUT 3000
#define AT_LONG_TIMEOUT 15000
#define HTTP_TIMEOUT 30000

//─── Power / location timer config ───────────────────────────────────────────
#define CHECK_INTERVAL_MS 30000UL     // check location every 30000 seconds
#define STATIONARY_LIMIT_MS 300000UL  // sleep after 5 min with no movement
#define SLEEP_DURATION_S 300          // wake from RTC sleep after 5 minutes
#define MOVE_THRESHOLD_M 10.0         // metres — below this = no movement

#define MIN_STEPS 20  // min steps/min to cancel sleep mode
#define IDLE_TIME 5
#define STEP_INACTIVITY_LIMIT_MS 300000UL  // sleep after 5 min with no steps
#define STEP_TIMER_INTERVAL_MS 60000UL     // STM32 timer fires every 1 minute
#define WAKE_MIN_DISTANCE_M 15.0
#define HTTP_REQ_DELAY 3000UL  //5000UL  // 5sec

//______ Variable

// ─── Network / server config ─ edit before flashing ──────────────────────────
#define APN "internet"                           // Operator APN
#define HTTP_SERVER "http://admin.stepwhere.in"  // Host (no http://)
#define HTTP_PORT 80
#define HTTPS_PORT 443
// #define HTTP_PATH "/api/location-store"  // Endpoint
// #define SERVER_CMD "AT+HTTPPARA=url,\"http://admin.stepwhere.in/api/location-store\""
// #define SERVER_CMD "AT+HTTPPARA=url,\"http://8545-106-219-71-174.ngrok-free.app/api/location-store\""

#define HTTP_PATH "/location"
#define SERVER_CMD "AT+HTTPPARA=url,\"http://ingest.stepwhere.in/location\""

#define HTTPS_SERVER_CMD "AT+HTTPSPARA=url,\"https://admin.stepwhere.in/api/location-store\""

#if 0
#define NEOWWAY_SERVER "AT+HTTPPARA=url,\"121.15.200.97/Service1.asmx/GetNote\""
#define NEOWAY_DATA "MAC=NEOWAY&DATA=0123456"
#else
#define NEOWWAY_SERVER "AT+HTTPPARA=url,\"http://www.example.com\""
#define NEOWAY_DATA "MAC=NEOWAY&DATA=0123456"
#endif

//"AT+HTTPPARA=url,\"http://admin.stepwhere.in/api/location-store\"", 1000

// ─── SSL / Certificate config ─────────────────────────────────────────────────
#define CA_CERT_FILENAME "ca_cert.pem"  // name stored on the module
#define CERT_UPLOAD_TIMEOUT 10000UL     // ms to wait for CONNECT / +CERTADD: OK

const char ROOT_CA[] = { 0 };

// ─── HTTPS server config — edit before flashing ───────────────────────────────
// #define HTTPS_SERVER  "admin.stepwhere.in"   // Host (no https://)
#define HTTPS_SERVER "011d-106-219-68-21.ngrok-free.app"  // Host (no https://)
#define HTTPS_PORT 443
#define HTTPS_PATH "/api/location-store"  // Endpoint
#define HTTPS_SSL_VERSION 3               // 0=SSL3.0 1=TLS1.0 2=TLS1.1 3=TLS1.2
#define HTTPS_AUTH_MODE 1                 // 0=no auth 1=server auth 2=mutual auth

// ─── Version ────────────────────────────────────────────────────────────────
#define APP_VERSION "v1.2-lis2dh12"  // test build: LIS2DH12 + software steps, no OTA
// ─── TinyGPS++ ────────────────────────────────────────────────────────────────
TinyGPSPlus gps;


//variable
String g_imei = "";
volatile uint32_t wake_count = 0;
uint8_t is_charging = false;
// ─── Forward declarations ─────────────────────────────────────────────────────

void ldo_enable(bool on);
void gsm_power_on();
void gsm_power_off();
bool gsm_send_at(const char *cmd, const char *expect,
                 uint32_t timeout_ms, String *response = nullptr);
bool modem_check();
String get_imei();
bool gprs_on();
bool http_post_json(const char *json_payload);
bool http_post(const char *json_payload);
bool https_post_location_json(const char *json_payload);
String build_location_telemetry_json();
String json_escape(const String &value);
bool https_post(const char *json_payload);
void gps_feed(uint32_t duration_ms);
void print_gps_info();
void build_json(char *buf, size_t buf_size, const String &imei);
bool neoway_test(const char *json_payload);
// ═════════════════════════════════════════════════════════════════════════════
//  POWER MANAGEMENT
// ═════════════════════════════════════════════════════════════════════════════

/** Enable / disable the LDO that powers the GSM & GPS modules */
void ldo_enable(bool on) {
  digitalWrite(LDO_EN, on ? HIGH : LOW);
  DEBUG.print("[LDO] ");
  DEBUG.println(on ? "ON" : "OFF");
  if (on) delay(100);  // let the rail stabilise
}

/**
 * Power ON — Neoway N706.
 * PWRKEY must be held LOW for ≥ 1 s while VBAT is present.
 */
void gsm_power_on() {
  DEBUG.println("[GSM] Power ON ...");
  ldo_enable(true);
  delay(500);  // LDO settle

  digitalWrite(GSM_PWRKEY, HIGH);
  delay(100);
  digitalWrite(GSM_PWRKEY, LOW);
  delay(1200);  // hold ≥ 1 s
  digitalWrite(GSM_PWRKEY, HIGH);

  DEBUG.println("[GSM] Waiting for boot ...");
  delay(3000);

  while (SerialGSM.available()) SerialGSM.read();  // flush boot noise
  DEBUG.println("[GSM] Power ON complete.");
}

/**
 * Power OFF — Neoway N706.
 * AT+CFUN=0 turns off radio and SIM power (N706 AT Manual §3.14).
 * Then cut the LDO.
 */
void gsm_power_off() {
  DEBUG.println("[GSM] Power OFF ...");
  gsm_send_at("AT+CFUN=0", "OK", 5000);
  delay(1000);
  // ldo_enable(false);
  DEBUG.println("[GSM] Power OFF complete.");
}


// =============================================================================
//  SYSTEM STATUS FLAGS
// =============================================================================
struct http_status {
  bool ok = false;
  uint8_t reconnect_cnt = 0;
  unsigned long last_send = 0;
} g_http_status;


struct SystemStatus {
  bool powered_on;      // LDO on + modem responded to AT
  bool imei_valid;      // IMEI is 15 digits and non-empty
  bool gprs_connected;  // AT+XIIC=1 succeeded and IP assigned
  bool is_gps_ready;

  // Returns true only when all three conditions are met
  bool ready() const {
    return powered_on && imei_valid && gprs_connected;
  }

  void reset() {
    powered_on = false;
    gprs_connected = false;
    is_gps_ready = false;
  }

  void print() const {
    INFO.println("[STATUS] ---- System Status ----");
    INFO.print("[STATUS] Powered ON   : ");
    INFO.println(powered_on ? "YES" : "NO");
    INFO.print("[STATUS] IMEI valid   : ");
    INFO.println(imei_valid ? "YES" : "NO");
    INFO.print("[STATUS] GPRS connect : ");
    INFO.println(gprs_connected ? "YES" : "NO");
    INFO.print("[STATUS] GPS        : ");
    INFO.println(is_gps_ready ? "YES" : "NO");
    INFO.print("[STATUS] Ready        : ");
    INFO.println(ready() ? "YES" : "NO");
    INFO.println("[STATUS] --------------------");
  }
} g_status;


// ─── Step / activity tracker ──────────────────────────────────────────────────
struct StepTracker {
  volatile uint32_t step_isr_count = 0;  // incremented in INT2 ISR
  uint32_t steps_this_min = 0;           // steps in last 1-min window
  uint32_t steps_total = 0;              // lifetime step total
  uint32_t previous_steps = 0;           // snapshot at last minute boundary
  uint16_t minute_counter = 0;           // uptime in minutes (wraps at 10000)
  unsigned long last_active_ms = 0;      // millis() of last step event
  bool active = false;                   // had steps in last window
  volatile bool sleep_mode_on = false;   // sleep flag
  uint8_t sleep_minute_count = 0;        // minutes elapsed since last wake-up
  double sleep_ref_lat = 0.0;            // GPS position at wake-up
  double sleep_ref_lon = 0.0;
  bool start_safe_shut_down = false;
  bool send_sleep_data = false;

  void mark_active() {
    last_active_ms = millis();
    active = true;
    sleep_mode_on = false;
  }
  bool is_inactive() {
    return (millis() - last_active_ms) >= STEP_INACTIVITY_LIMIT_MS;
  }
} g_steps;

// ─── Location / sleep tracker ─────────────────────────────────────────────────
struct LocationTracker {
  double last_lat = 0.0;
  double last_lon = 0.0;
  double current_lat = 0.0;
  double current_lon = 0.0;
  int num_satellite = 0;
  unsigned long last_check_ms = 0;     // millis() of last 1-min check
  unsigned long stationary_since = 0;  // millis() when movement stopped
  bool has_fix = false;                // at least one valid fix stored to store
  bool is_stationary = false;          // currently below threshold

  void update_reference(double lat, double lon) {
    last_lat = lat;
    last_lon = lon;
    has_fix = true;
    stationary_since = millis();
    is_stationary = false;
  }
} g_loc;

volatile bool g_double_tap = false;  // set in INT1 ISR, cleared in loop()
// ═════════════════════════════════════════════════════════════════════════════
//  AT COMMAND HELPER
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Send an AT command and wait for an expected substring in the response.
 *
 * @param cmd        Command string — \r\n is appended automatically.
 *                   Pass "" to only listen (e.g. after sending raw body bytes).
 * @param expect     Substring that marks success (e.g. "OK", "+XIIC:")
 * @param timeout_ms Milliseconds to wait
 * @param response   Optional: receives the full reply
 * @return true if expect was found within timeout
 */
#if 0
bool gsm_send_at(const char *cmd, const char *expect,
                 uint32_t timeout_ms, String *response) {

    // while (SerialGSM.available()) SerialGSM.read();   // flush RX

    if (cmd[0] != '\0') {
        DEBUG.print("[AT] >> ");
        DEBUG.println(cmd);
        SerialGSM.print(cmd);
        SerialGSM.print("\r\n");
    }

    String reply  = "";
    unsigned long t0   = millis();
    char c;
    uint8_t linecount = 0;
    unsigned long  dot  = millis();
    uint8_t dataReceived = 0;
    unsigned long  lastDataTime  = millis();
    while (millis() - t0 < timeout_ms) {
        while (SerialGSM.available())
        {
            c = (char)SerialGSM.read();
            reply += c;
            if(c == '\n')  linecount++;

            if(c == '>') {
                DEBUG.print("[AT] << ");
                DEBUG.println(reply);
                if (response) *response = reply;
                return true;
            }

            if(linecount >= 2)
            {
                dataReceived = 1;
                lastDataTime = millis();
            }
        }


        if(millis() - dot > 1000)
        {
            dot  = millis();
            DEBUG.print(".");
        }
         // If we've started receiving and it’s been quiet for 200 ms → done
        if (dataReceived && (millis() - lastDataTime > 2000)) {
            if (reply.indexOf(expect) != -1) {
                DEBUG.print("[AT] << ");
                DEBUG.println(reply);
                if (response) *response = reply;
                return true;
            }
            if (reply.indexOf("ERROR") != -1) break;
        }
    }

    DEBUG.print("[AT] FAIL — got: ");
    DEBUG.println(reply);
    if (response) *response = reply;
    return false;
}
#else

bool gsm_send_at(const char *cmd,
                 const char *expect,
                 uint32_t timeout_ms,
                 String *response) {

  while (SerialGSM.available()) SerialGSM.read();
  if (cmd && cmd[0] != '\0') {
    DEBUG.print("[AT][CMD] >> ");
    DEBUG.println(cmd);
    SerialGSM.print(cmd);
    SerialGSM.print("\r\n");
    delay(100);
  }

  static String reply;
  reply = "";
  reply.reserve(128);
  uint8_t data_count = 0;

  unsigned long start = millis();
  unsigned long lastRx = millis();
  unsigned long dot = millis();
  bool ret_code = false;

  while ((millis() - start) < timeout_ms) {
    while (SerialGSM.available()) {
      char c = (char)SerialGSM.read();
      reply += c;
      data_count++;
      lastRx = millis();

      // Prompt for data mode
      if (c == '>') {
        // DEBUG.print("[AT] << ");
        // DEBUG.println(reply);
        if (response)
          *response = reply;
        ret_code = true;
        break;
        //return true;
      }

      if (expect && reply.indexOf(expect) != -1) {
        // DEBUG.print("[AT] << ");
        // DEBUG.println(reply);
        if (response)
          *response = reply;
        ret_code = true;
        break;
        // return true;
      }  // Failure immediately
      else if (reply.indexOf("ERROR") != -1) {
        // DEBUG.print("[AT] << ");
        // DEBUG.println(reply);
        if (response)
          *response = reply;
        ret_code = false;
        break;
        // return false;
      }
    }
    // Response complete after modem becomes quiet
    if ((data_count > 127) || (reply.length() > 0 && (millis() - lastRx) > 200)) {
      if (expect && reply.indexOf(expect) != -1) {
        // DEBUG.print("[AT] << ");
        // DEBUG.println(reply);
        if (response)
          *response = reply;
        ret_code = true;
        break;
        // return true;
      }
    }
    // print dot if no response
    if (millis() - dot > 1000) {
      dot = millis();
      DEBUG.print(".");
    }
  }

  DEBUG.print("[AT][RSP] << ");
  DEBUG.println(reply);
  if (response)
    *response = reply;
  return ret_code;
}
#endif

// ═════════════════════════════════════════════════════════════════════════════
//  MODEM CHECK  (AT + AT+CPIN?)
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Verify the modem is alive and the SIM is ready.
 * Retries AT up to 5 times to allow for slow boot.
 */
bool modem_check() {
  DEBUG.println("[Modem] Checking AT ...");

  bool at_ok = false;
  for (int i = 0; i < 5; i++) {
    if (gsm_send_at("AT", "OK", AT_TIMEOUT)) {
      at_ok = true;
      break;
    }
    DEBUG.print("[Modem] Retry ");
    DEBUG.println(i + 1);
    delay(1000);
  }
  if (!at_ok) {
    DEBUG.println("[Modem] ERROR: No response to AT");
    return false;
  }

  gsm_send_at("ATE0", "OK", AT_TIMEOUT);

  DEBUG.println("[Modem] Checking SIM (AT+CPIN?) ...");
  String reply;
  if (!gsm_send_at("AT+CPIN?", "+CPIN:", AT_TIMEOUT, &reply)) {
    DEBUG.println("[Modem] ERROR: No CPIN response");
    return false;
  } else {
    // return true;
    if (reply.indexOf("READY") != -1) {
      DEBUG.println("[Modem] SIM READY");
      return true;
    }
  }
  // if      (reply.indexOf("SIM PIN") != -1) DEBUG.println("[Modem] ERROR: SIM PIN required");
  // else if (reply.indexOf("SIM PUK") != -1) DEBUG.println("[Modem] ERROR: SIM PUK required");
  // else                                      DEBUG.println("[Modem] ERROR: SIM not ready");
  return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  IMEI  (AT+CGSN)
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @return 15-digit IMEI string, or "" on failure.
 */
String get_imei() {
  String reply;
  if (!gsm_send_at("AT+CGSN", "OK", AT_TIMEOUT, &reply)) {
    DEBUG.println("[IMEI] ERROR");
    return "";
  }
  // Raw reply: \r\nAT+CGSN\r\n\r\n<15 digits>\r\n\r\nOK\r\n
  // Strip echo
  int echo = reply.indexOf("+CGSN: ");
  if (echo != -1) reply = reply.substring(echo + 7);
  // Strip OK
  int ok = reply.lastIndexOf("OK");
  if (ok != -1) reply = reply.substring(0, ok);
  reply.trim();

  DEBUG.print("[IMEI] ");
  DEBUG.println(reply);
  return reply;
}

// ═════════════════════════════════════════════════════════════════════════════
//  GPRS ON  (AT+CGDCONT → AT+XIIC=1)
//  N706 AT Manual §5.2: AT+CGDCONT must be set before AT+XIIC=1
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Bring up GPRS bearer.
 *
 * Per the manual:
 *   1. AT+CGDCONT=1,"IP","<apn>"   — configure PDP context
 *   2. AT+XIIC=1                   — activate PPP link  → OK
 *   3. AT+XIIC?                    → +XIIC: 1,<ip>      — confirm IP
 */
bool gprs_on() {
  INFO.println("[GPRS] Configuring APN (AT+CGDCONT) ...");

  char cmd[80];
  snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    ERR.println("[GPRS] ERROR: AT+CGDCONT failed");
    return false;
  }

  // Close any existing PPP link first (ignore errors)
  gsm_send_at("AT+XIIC=0", "OK", AT_TIMEOUT);
  delay(500);

  DEBUG.println("[GPRS] Activating PPP link (AT+XIIC=1) ...");
  if (!gsm_send_at("AT+XIIC=1", "OK", AT_LONG_TIMEOUT)) {
    ERR.println("[GPRS] ERROR: AT+XIIC=1 failed");
    return false;
  }
  delay(1000);  // let IP assignment settle

  // Verify connection and read assigned IP
  String reply;
  if (!gsm_send_at("AT+XIIC?", "+XIIC:", 5000, &reply)) {
    ERR.println("[GPRS] ERROR: AT+XIIC? no response");
    return false;
  }

  // +XIIC: <state>,<ip>   state: 1 = connected
  int idx = reply.indexOf("+XIIC:");
  if (idx == -1) {
    ERR.println("[GPRS] ERROR: unexpected format");
    return false;
  }
  INFO.println(reply.c_str());
  // String val = reply.substring(idx + 4);
  // val.trim();
  // // Manual shows four spaces before state digit: "+XIIC:    1,10.107.216.162"
  // val.trim();   // trim again after removing prefix

  // if (val.charAt(0) != '1') {
  //     DEBUG.println("[GPRS] ERROR: PPP not connected (+XIIC state != 1)");
  //     return false;
  // }
  // int comma = val.indexOf(',');
  // String ip = (comma != -1) ? val.substring(comma + 1) : "?";
  // ip.trim();
  int Index1 = reply.indexOf(',');
  int Index2 = reply.indexOf('\r', Index1 + 1);
  String ip = reply.substring(Index1 + 1, Index2);
  ip.trim();
  // if ((ip.indexOf("0.0.0.0") == -1) && (ip.indexOf("OK") == -1)) {
  // //    DEBUG.println("**IP:");
  // //    DEBUG.println(ip);
  // return ip;
  // } else {
  // return "IP not found error";
  // }

  INFO.print("[GPRS] IP: ");
  INFO.println(ip);
  INFO.println("[GPRS] GPRS UP");
  return true;
}


/**
 * Read a CONNECT-prompt response from SerialGSM and then send raw bytes.
 * Used by cert_upload() — the module behaves like a file-upload prompt:
 *   AT+CERTADD=<file_name>,<length>
 *   CONNECT                       ← module is ready for raw bytes
 *   <send exactly <length> bytes>
 *   +CERTADD: <length>,OK         ← success
 *   or
 *   +CERTADD: ERROR / ERROR       ← failure
 */
bool cert_upload(const char *filename, const char *pem_progmem, size_t pem_len) {
  DEBUG.print("[CERT] Uploading ");
  DEBUG.print(filename);
  DEBUG.print(" (");
  DEBUG.print(pem_len);
  DEBUG.println(" bytes) ...");

  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+CERTADD=%s,%u", filename, (unsigned)pem_len);

  // Send the command and wait for the CONNECT prompt
  if (!gsm_send_at(cmd, "CONNECT", CERT_UPLOAD_TIMEOUT)) {
    DEBUG.println("[CERT] ERROR: no CONNECT prompt — upload aborted");
    return false;
  }

  // Stream the PEM bytes from PROGMEM directly to the modem UART
  for (size_t i = 0; i < pem_len; i++) {
    char c = pem_progmem[i];
    SerialGSM.write(c);
  }

  // Wait for the final +CERTADD: <len>,OK confirmation
  String reply;
  bool ok = gsm_send_at("", "+CERTADD:", CERT_UPLOAD_TIMEOUT, &reply);

  if (!ok || reply.indexOf("OK") == -1) {
    DEBUG.print("[CERT] ERROR: upload failed — got: ");
    DEBUG.println(reply);
    return false;
  }

  DEBUG.println("[CERT] Upload OK.");
  return true;
}

/**
 * Convenience wrapper to upload the CA cert defined above.
 */
bool cert_upload_ca() {
  size_t len = strlen(ROOT_CA);
  cert_delete(CA_CERT_FILENAME);
  return cert_upload(CA_CERT_FILENAME, ROOT_CA, len);
}

/**
 * Check whether a certificate file already exists on the module.
 * AT+CERTCHECK=<file_name>  →  +CERTCHECK: <file_name>,OK  (exists)
 *                            or +CERTCHECK: ERROR           (does not exist)
 */
bool cert_check(const char *filename) {
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+CERTCHECK=%s", filename);

  String reply;
  gsm_send_at(cmd, "CERTCHECK", AT_TIMEOUT, &reply);

  bool exists = (reply.indexOf("OK") != -1) && (reply.indexOf("ERROR") == -1);
  DEBUG.print("[CERT] ");
  DEBUG.print(filename);
  DEBUG.println(exists ? " — exists" : " — not found");
  return exists;
}

/**
 * Delete a certificate by name, or all certificates if filename is "".
 */
bool cert_delete(const char *filename) {
  char cmd[64];
  if (filename[0] != '\0') {
    snprintf(cmd, sizeof(cmd), "AT+CERTDEL=%s", filename);
  } else {
    snprintf(cmd, sizeof(cmd), "AT+CERTDEL");
  }
  return gsm_send_at(cmd, "OK", AT_TIMEOUT);
}

/**
 * Ensure the CA certificate is present on the module — uploads it only if
 * it is not already there. Call once from setup() before using HTTPS.
 */
bool cert_ensure_ca_present() {
  DEBUG.println("[CERT] Checking for CA certificate on module ...");
  if (cert_check(CA_CERT_FILENAME)) {
    DEBUG.println("[CERT] CA certificate already present — skipping upload.");
    return true;
  }
  DEBUG.println("[CERT] CA certificate not found — uploading now.");
  return cert_upload_ca();
}

/**
 * Configure SSL parameters for HTTPS — points the module at the CA cert
 * uploaded via cert_ensure_ca_present(). Call once after GPRS/cert are up,
 * before the first https_post().
 *
 * Sequence (manual §8.10):
 *   AT+HTTPSCFG="sslversion",<3>     → TLS1.2
 *   AT+HTTPSCFG="authmode",<1>       → server auth only (use 2 for mutual TLS)
 *   AT+HTTPSCFG="cacert",ca_cert.pem → CA cert must already be uploaded
 */
bool https_configure_ssl() {
  DEBUG.println("[HTTPS] Configuring SSL parameters ...");
  char cmd[64];

  snprintf(cmd, sizeof(cmd), "AT+HTTPSCFG=\"sslversion\",%d", HTTPS_SSL_VERSION);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: sslversion config failed");
    return false;
  }

  snprintf(cmd, sizeof(cmd), "AT+HTTPSCFG=\"authmode\",%d", HTTPS_AUTH_MODE);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: authmode config failed");
    return false;
  }

  // Only attach the CA cert if authentication is enabled
  if (HTTPS_AUTH_MODE != 0) {
    snprintf(cmd, sizeof(cmd), "AT+HTTPSCFG=\"cacert\",%s", CA_CERT_FILENAME);
    if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
      DEBUG.println("[HTTPS] ERROR: cacert config failed");
      return false;
    }
  }
  DEBUG.println("[HTTPS] SSL configuration OK.");
  return true;
}

/**
 * Perform an HTTPS POST with a JSON body.
 *
 * Sequence (manual §8.11-8.14):
 *   AT+HTTPSPARA=url,"<host><path>"
 *   AT+HTTPSPARA=port,<port>
 *   AT+HTTPSSETUP                       → OK  (requires AT+XIIC=1 already up)
 *   AT+HTTPSACTION=2,<len>,2            → OK then ">" prompt (2=POST, type 2=json)
 *   <send body bytes>
 *                                       → +HTTPSRECV: <raw response>
 *                                       → +HTTPSCLOSED: HTTPS Link Closed
 *   AT+HTTPSCLOSE                       → OK
 *
 * @return true if the server responded (+HTTPSRECV: received)
 */
bool https_post_v1(const char *json_payload) {
  DEBUG.println("[HTTPS] Starting HTTPS POST ...");

  size_t payload_len = strlen(json_payload);
  char cmd[200];

  // ── 1) Set URL ────────────────────────────────────────────────────────────
  snprintf(cmd, sizeof(cmd),
           "AT+HTTPSPARA=url,\"%s%s\"", HTTPS_SERVER, HTTPS_PATH);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: HTTPSPARA url failed");
    return false;
  }

  // ── 2) Set port ───────────────────────────────────────────────────────────
  snprintf(cmd, sizeof(cmd), "AT+HTTPSPARA=port,%d", HTTPS_PORT);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: HTTPSPARA port failed");
    return false;
  }

  // ── 3) Open HTTPS connection ──────────────────────────────────────────────
  if (!gsm_send_at("AT+HTTPSSETUP", "OK", HTTP_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: HTTPSSETUP failed");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // ── 4) POST — mode=2, length, type=2(json) ────────────────────────────────
  snprintf(cmd, sizeof(cmd), "AT+HTTPSACTION=2,%u,2", (unsigned)payload_len);
  if (!gsm_send_at(cmd, ">", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: HTTPSACTION no > prompt");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // ── 5) Send body ──────────────────────────────────────────────────────────
  DEBUG.print("[HTTPS] Sending body: ");
  DEBUG.println(json_payload);
  SerialGSM.print(json_payload);

  // ── 6) Wait for server response (+HTTPSRECV:) ─────────────────────────────
  String resp;
  bool got_recv = gsm_send_at("", "+HTTPSRECV:", HTTP_TIMEOUT, &resp);

  if (!got_recv) {
    DEBUG.println("[HTTPS] ERROR: no +HTTPSRECV response");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // Collect until +HTTPSCLOSED: URC or timeout
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPSCLOSED:") != -1) break;
  }
  DEBUG.print("[HTTPS] Response: ");
  DEBUG.println(resp);

  // ── 7) Close HTTPS socket (HTTPSPARA settings are kept per §8.14) ────────
  gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);

  DEBUG.println("[HTTPS] POST complete.");
  return true;
}

bool https_post_location_json(const char *json_payload) {
  DEBUG.println("[HTTPS] Starting telemetry JSON POST ...");

  size_t payload_len = strlen(json_payload);
  char cmd[200];

  snprintf(cmd, sizeof(cmd),
           "AT+HTTPSPARA=url,\"%s%s\"", HTTPS_SERVER, HTTPS_PATH);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: telemetry HTTPSPARA url failed");
    return false;
  }

  snprintf(cmd, sizeof(cmd), "AT+HTTPSPARA=port,%d", HTTPS_PORT);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: telemetry HTTPSPARA port failed");
    return false;
  }

  if (!gsm_send_at("AT+HTTPSSETUP", "OK", HTTP_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: telemetry HTTPSSETUP failed");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  snprintf(cmd, sizeof(cmd), "AT+HTTPSACTION=2,%u,2", (unsigned)payload_len);
  if (!gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTPS] ERROR: telemetry HTTPSACTION no > prompt");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  DEBUG.print("[HTTPS] Sending telemetry JSON: ");
  DEBUG.println(json_payload);
  SerialGSM.print(json_payload);

  String resp;
  bool got_recv = gsm_send_at("", "+HTTPSRECV:", HTTP_TIMEOUT, &resp);

  if (!got_recv) {
    DEBUG.println("[HTTPS] ERROR: telemetry no +HTTPSRECV response");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPSCLOSED:") != -1) break;
  }

  DEBUG.print("[HTTPS] Telemetry response: ");
  DEBUG.println(resp);

  gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);

  bool accepted = (resp.indexOf("HTTP/1.1 201") != -1) || (resp.indexOf("HTTP/1.0 201") != -1) || (resp.indexOf(" 201 ") != -1);

  if (!accepted) {
    DEBUG.println("[HTTPS] ERROR: telemetry POST did not return 201");
  }

  return accepted;
}





// ═════════════════════════════════════════════════════════════════════════════
//  HTTP POST
//  N706 AT Manual §8.1–8.4
//
//  Sequence (from manual POST example, §8.3):
//    AT+HTTPPARA=url,"<host>/<path>"
//    AT+HTTPPARA=port,<port>
//    AT+HTTPSETUP               → OK  (or +HTTPSETUP: FAIL)
//    AT+HTTPACTION=2,<len>,2    → OK  then ">" prompt
//    <send body bytes>
//    (server replies via +HTTPRECV: URC)
//    (connection closed via +HTTPCLOSED: HTTP Link Closed URC)
//    AT+HTTPCLOSE               → OK
//
//  POST type codes (§8.3): 0=form-urlencoded, 1=text, 2=json, 3=xml, 4=html
// ═════════════════════════════════════════════════════════════════════════════
/**
 * Perform HTTP POST with a JSON body.
 * @return true if the server replied (i.e. +HTTPRECV: received)
 */
bool http_post_json(const char *json_payload) {
  DEBUG.println("[HTTP] Starting ingest JSON POST ...");

  size_t payload_len = strlen(json_payload);
  char cmd[220];

  if (!gsm_send_at(SERVER_CMD, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPARA url failed");
    return false;
  }

  snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=port,%d", HTTP_PORT);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPARA port failed");
    return false;
  }

  if (!gsm_send_at("AT+HTTPSETUP", "OK", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPSETUP failed");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  snprintf(cmd, sizeof(cmd), "AT+HTTPACTION=2,%u,2", (unsigned)payload_len);
  if (!gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPACTION no > prompt");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  DEBUG.print("[HTTP] Sending ingest JSON: ");
  DEBUG.println(json_payload);
  SerialGSM.print(json_payload);

  String resp;
  bool got_recv = gsm_send_at("", "+HTTPRECV:", HTTP_TIMEOUT, &resp);

  if (!got_recv) {
    DEBUG.println("[HTTP] ERROR: no +HTTPRECV response");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPCLOSED:") != -1) break;
  }

  INFO.print("[HTTP] Ingest response: ");
  INFO.println(resp);

  gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);

  bool accepted =
    (resp.indexOf("HTTP/1.1 201") != -1) || (resp.indexOf("HTTP/1.0 201") != -1) || (resp.indexOf(" 201 ") != -1);

  if (!accepted) {
    DEBUG.println("[HTTP] ERROR: ingest POST did not return 201");
  }

  return accepted;
}

/**
 * Perform HTTP POST with a JSON body.
 * @return true if the server replied (i.e. +HTTPRECV: received)
 */
bool http_post(const char *json_payload) {
  DEBUG.println("[HTTP] Starting HTTP POST ...");

  size_t payload_len = strlen(json_payload);
  char cmd[200];

  // ── 1) Set URL (host + path, no scheme prefix per manual examples) ────────
  //   snprintf(cmd, sizeof(cmd),"AT+HTTPPARA=url,\"%s%s\"", HTTP_SERVER, HTTP_PATH);
  String CMD = SERVER_CMD;
  if (!gsm_send_at(CMD.c_str(), "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPARA url failed");
    return false;
  }

  // // ── 2) Set port ───────────────────────────────────────────────────────────
  // snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=port,%d", HTTP_PORT);
  // if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
  //     DEBUG.println("[HTTP] ERROR: HTTPPARA port failed");
  //     return false;
  // }

  // ── 3) Open HTTP connection ───────────────────────────────────────────────
  // Success → OK ; Failure → +HTTPSETUP: FAIL  (§8.2)
  if (!gsm_send_at("AT+HTTPSETUP", "OK", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPSETUP failed");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);

    return false;
  }
  // ── 4) POST — mode=2, length, type=2(json) ────────────────────────────────
  // After OK the modem outputs ">" and waits for body bytes (§8.3 example)

  snprintf(cmd, sizeof(cmd), "AT+HTTPACTION=2,%d", (unsigned)payload_len + 2);
  if (!gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPACTION no > prompt");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // ── 5) Send body ──────────────────────────────────────────────────────────
  DEBUG.print("[HTTP] Sending body: ");
  DEBUG.println(json_payload);

  // ── 6) Wait for server response (+HTTPRECV:) and connection close ─────────
  // The module emits +HTTPRECV: <headers+body> then +HTTPCLOSED: HTTP Link Closed
  String resp;
  gsm_send_at(json_payload, "OK", HTTP_TIMEOUT, &resp);
  if (resp.indexOf("OK") == -1) {

    DEBUG.println("[HTTP] ERROR: no +Ok response");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // Collect until +HTTPCLOSED URC or timeout
  resp = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPCLOSED:") != -1) break;
  }
  DEBUG.print("[HTTP] Response: ");
  DEBUG.println(resp);

  // ── 7) Close HTTP socket (clears HTTPPARA settings per §8.4) ─────────────
  gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
  DEBUG.println("[HTTP] POST complete.");
  return true;
}


/**
 * Perform HTTP POST with a JSON body.
 * @return true if the server replied (i.e. +HTTPRECV: received)
 */
bool https_post(const char *json_payload) {
  DEBUG.println("[HTTP] Starting HTTP POST ...");

  size_t payload_len = strlen(json_payload);
  char cmd[200];

  // AT+HTTPSCFG=”sslversion”,0
  //   OK
  // Set the SSL version to SSL3.0.


  if (!gsm_send_at("AT+HTTPSCFG?", "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPSCFG?  failed");
    return false;
  }
  if (!gsm_send_at("AT+HTTPSCFG=\"sslversion\",3", "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: sslversion  failed");
    return false;
  }

  // AT + HTTPSCFG ="authmode", 0

  if (!gsm_send_at("AT+HTTPSCFG=\"authmode\",0", "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPSCFG  failed");
    return false;
  }

  // ── 1) Set URL (host + path, no scheme prefix per manual examples) ────────
  //   snprintf(cmd, sizeof(cmd),"AT+HTTPSPARA=url,\"%s%s\"", HTTP_SERVER, HTTP_PATH);
  String CMD = "AT+HTTPSPARA=url,\"https://admin.stepwhere.in/api/location-store\"";
  if (!gsm_send_at(CMD.c_str(), "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPSARA url failed");
    return false;
  }

  // ── 2) Set port ───────────────────────────────────────────────────────────
  snprintf(cmd, sizeof(cmd), "AT+HTTPSPARA=port,%d", HTTPS_PORT);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPARA port failed");
    return false;
  }

  // ── 3) Open HTTP connection ───────────────────────────────────────────────
  // Success → OK ; Failure → +HTTPSETUP: FAIL  (§8.2)
  if (!gsm_send_at("AT+HTTPSSETUP", "OK", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPSETUP failed");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);

    return false;
  }
  // ── 4) POST — mode=2, length, type=2(json) ────────────────────────────────
  // After OK the modem outputs ">" and waits for body bytes (§8.3 example)

  snprintf(cmd, sizeof(cmd), "AT+HTTPSACTION=2,%d,0", (unsigned)payload_len + 2);
  if (!gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPACTION no > prompt");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // ── 5) Send body ──────────────────────────────────────────────────────────
  DEBUG.print("[HTTP] Sending body: ");
  DEBUG.println(json_payload);

  // ── 6) Wait for server response (+HTTPRECV:) and connection close ─────────
  // The module emits +HTTPRECV: <headers+body> then +HTTPCLOSED: HTTP Link Closed
  String resp;
  gsm_send_at(json_payload, "OK", HTTP_TIMEOUT, &resp);
  if (resp.indexOf("OK") == -1) {

    DEBUG.println("[HTTP] ERROR: no +Ok response");
    gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // Collect until +HTTPCLOSED URC or timeout
  resp = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPSCLOSED:") != -1) break;
  }
  DEBUG.print("[HTTP] Response: ");
  DEBUG.println(resp);

  // ── 7) Close HTTP socket (clears HTTPPARA settings per §8.4) ─────────────
  gsm_send_at("AT+HTTPSCLOSE", "OK", AT_TIMEOUT);
  DEBUG.println("[HTTP] POST complete.");
  return true;
}

void print_res() {
  String resp = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPCLOSED:") != -1) break;
  }
  DEBUG.print("[HTTP] Response: ");
  DEBUG.println(resp);
}

bool neoway_test(const char *json_payload) {


#if 1
  DEBUG.println("[HTTP] Starting HTTP POST ...");

  size_t payload_len = strlen(json_payload);
  char cmd[200];

  // ── 1) Set URL (host + path, no scheme prefix per manual examples) ────────
  //   snprintf(cmd, sizeof(cmd),"AT+HTTPPARA=url,\"%s%s\"", HTTP_SERVER, HTTP_PATH);
  String CMD = NEOWWAY_SERVER;
  if (!gsm_send_at(CMD.c_str(), "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPARA url failed");
    return false;
  }

  // // ── 2) Set port ───────────────────────────────────────────────────────────
  snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=port,%d", 80);
  if (!gsm_send_at(cmd, "OK", AT_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPPARA port failed");
    return false;
  }

  // ── 3) Open HTTP connection ───────────────────────────────────────────────
  // Success → OK ; Failure → +HTTPSETUP: FAIL  (§8.2)
  if (!gsm_send_at("AT+HTTPSETUP", "OK", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPSETUP failed");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }

  // ── 4) POST — mode=2, length, type=2(json) ────────────────────────────────
  // After OK the modem outputs ">" and waits for body bytes (§8.3 example)

  // snprintf(cmd, sizeof(cmd), "AT+HTTPACTION=2,%d", (unsigned)payload_len+2);
  snprintf(cmd, sizeof(cmd), "AT+HTTPACTION=0");
  if (!gsm_send_at(cmd, ">", AT_LONG_TIMEOUT)) {
    DEBUG.println("[HTTP] ERROR: HTTPACTION no > prompt");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }
  /*
  // ── 5) Send body ──────────────────────────────────────────────────────────
  DEBUG.print("[HTTP] Sending body: ");
  DEBUG.println(json_payload);

  // ── 6) Wait for server response (+HTTPRECV:) and connection close ─────────
  // The module emits +HTTPRECV: <headers+body> then +HTTPCLOSED: HTTP Link Closed
  String resp;
  gsm_send_at(json_payload, "OK", HTTP_TIMEOUT, &resp);
  if (resp.indexOf("OK") == -1) {

    DEBUG.println("[HTTP] ERROR: no +Ok response");
    gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
    return false;
  }
*/
  // Collect until +HTTPCLOSED URC or timeout




  String resp = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (SerialGSM.available()) resp += (char)SerialGSM.read();
    if (resp.indexOf("+HTTPCLOSED:") != -1) break;
  }
  DEBUG.print("[HTTP] Response: ");
  DEBUG.println(resp);

  // ── 7) Close HTTP socket (clears HTTPPARA settings per §8.4) ─────────────
  gsm_send_at("AT+HTTPCLOSE", "OK", AT_TIMEOUT);
  DEBUG.println("[HTTP] POST complete.");
  return true;
#else

  SerialGSM.println("AT+HTTPPARA=url,\"121.15.200.97/Service1.asmx/GetNote\"");
  print_res();
  SerialGSM.println("AT+HTTPPARA=port,8080");
  print_res();

  SerialGSM.println("AT+HTTPSETUP");
  print_res();

  SerialGSM.println("AT+HTTPACTION=2,25");
  print_res();
  SerialGSM.println("MAC=NEOWAY&DATA=0123456");
  print_res();
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
//  GPS HELPERS
// ═════════════════════════════════════════════════════════════════════════════

void gps_check_timeout() {
  bool validFix =
    gps.location.isValid() && gps.satellites.isValid() && (gps.satellites.value() > MIN_SAT);


  if (validFix) {
    gpsNoFixTimerRunning = false;
    g_status.is_gps_ready = 1;
    return;
  }

  if (!gpsNoFixTimerRunning) {
    gpsNoFixStart = millis();
    gpsNoFixTimerRunning = true;

    DEBUG.println(
      "[GPS] No valid fix - timer started");

    return;
  }


  if (millis() - gpsNoFixStart >= GPS_NO_FIX_TIMEOUT_MS) {
    DEBUG.println(
      "[GPS] No valid fix for 10 minutes System restart");
    ldo_enable(false);
    gpsNoFixTimerRunning = false;
    gpsNoFixStart = millis();
    delay(2000);
    gpsreset_magic = GPS_RESET_MAGIC;
    NVIC_SystemReset();
    while (1)
      ;
  }
}

/** Feed NMEA bytes into TinyGPSPlus for duration_ms milliseconds */
void gps_feed(uint32_t duration_ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < duration_ms) {
    while (SerialGPS.available()) gps.encode(SerialGPS.read());
    delay(10);
  }

  g_status.is_gps_ready = 0;
  // g_loc.current_lat = 0.0f;
  // g_loc.current_lon = 0.0f;
  // g_loc.num_satellite = 0;
   
  if (!gps.location.isValid()) {
    DEBUG.println("[GPS] No valid GPS fix — skipping location check.");
    return;
  }

  if (!gps.location.isUpdated())
    return;

  if (!gps.satellites.isValid())
    return;

  g_loc.num_satellite = gps.satellites.value();
  g_loc.current_lat = gps.location.lat();
  g_loc.current_lon = gps.location.lng();
  if (g_loc.num_satellite > MIN_SAT) {
    if (!g_loc.has_fix) {
      g_steps.sleep_ref_lat = g_loc.current_lat;  // update the sleep lat & lon at first
      g_steps.sleep_ref_lon = g_loc.current_lon;
      g_loc.update_reference(g_loc.current_lat, g_loc.current_lon);
      g_loc.has_fix = true;
    }
    g_status.is_gps_ready = 1;
  } else {
    g_status.is_gps_ready = 0;
  }
  DEBUG.print("[GPS] Lat: ");
  DEBUG.print(g_loc.current_lat, 6);
  DEBUG.print("  Lon: ");
  DEBUG.println(g_loc.current_lon, 6);
  DEBUG.print("[GPS] Sats: ");
  DEBUG.println(g_loc.num_satellite);
  /*
  if (gps.speed.isValid()) {
    DEBUG.print("[GPS] Speed km/h: ");
    DEBUG.println(gps.speed.kmph(), 1);
  }
  if (gps.altitude.isValid()) {
    DEBUG.print("[GPS] Alt m: ");
    DEBUG.println(gps.altitude.meters(), 1);
  }
  if (gps.date.isValid() && gps.time.isValid()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "[GPS] UTC %04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    DEBUG.println(buf);
  }
  */
}

/** Print current fix to debug console */
void print_gps_info() {
  if (g_status.is_gps_ready) {
    INFO.print("[GPS] Lat: ");
    INFO.print(g_loc.current_lat, 6);
    INFO.print("  Lon: ");
    INFO.println(g_loc.current_lon, 6);
    INFO.print("[GPS] Sats: ");
    INFO.println(g_loc.num_satellite);
  } else {
    INFO.println("[GPS] Location: INVALID");
    return;
  }
  if (gps.date.isValid() && gps.time.isValid()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "[GPS] UTC %04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    INFO.println(buf);
  }
  /*
  if (gps.speed.isValid()) {
    INFO.print("[GPS] Speed km/h: ");
    INFO.println(gps.speed.kmph(), 1);
  }
  if (gps.altitude.isValid()) {
    INFO.print("[GPS] Alt m: ");
    INFO.println(gps.altitude.meters(), 1);
  }
  if (gps.date.isValid() && gps.time.isValid()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "[GPS] UTC %04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    INFO.println(buf);
  }
  INFO.print("[GPS] Sats: ");
  INFO.println(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
  */
}

// ═════════════════════════════════════════════════════════════════════════════
//  JSON BUILDER
// ═════════════════════════════════════════════════════════════════════════════

void build_json(char *buf, size_t buf_size, const String &imei) {
  double lat = g_loc.current_lat;  //gps.location.isValid() ? gps.location.lat() : 0.0;
  double lon = g_loc.current_lon;  //gps.location.isValid() ? gps.location.lng() : 0.0;
  // double spd = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  // double alt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  int sat = g_loc.num_satellite;  //gps.satellites.isValid() ? (int)gps.satellites.value() : 0;

  snprintf(buf, buf_size,
           "{\"imei\":\"%s\","
           "\"lat\":%.6f,\"lon\":%.6f,"
           /* "\"speed\":%.1f,\"alt\":%.1f,"*/
           "\"sat\":%d,\"valid\":%s}",
           imei.c_str(), lat, lon, /*spd, alt,*/ sat,
           gps.location.isValid() ? "true" : "false");
}

String json_escape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    switch (c) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          escaped += buf;
        } else {
          escaped += c;
        }
        break;
    }
  }
  return escaped;
}

String build_location_telemetry_json() {
  String hardware_id = g_imei;
  hardware_id.trim();
  if (hardware_id.length() == 0) hardware_id = "<NO_DATA>";

  double lat = g_loc.current_lat; //g_loc.has_fix ? g_loc.current_lat : 0.0;
  double lon = g_loc.current_lon; // g_loc.has_fix ? g_loc.current_lon : 0.0;

  String dev_status = g_steps.sleep_mode_on ? "sleep" : "active";

  int battery_percent = g_vdd_mon.battery_level;
  if (battery_percent < 0) battery_percent = 0;
  if (battery_percent > 100) battery_percent = 100;

  int battery_mv = 1;
  if (g_vdd_mon.bat_volt > 0.0f) {
    battery_mv = (int)(g_vdd_mon.bat_volt * 1000.0f + 0.5f);
    if (battery_mv < 1) battery_mv = 1;
  }

  int satellite_count = g_loc.num_satellite;
  if (satellite_count < 0) satellite_count = 0;

  String body;
  body.reserve(320);
  body += "{";
  body += "\"hardwareIdentifier\":\"";
  body += json_escape(hardware_id);
  body += "\",";
  body += "\"latitude\":";
  body += String(lat, 8);
  body += ",";
  body += "\"longitude\":";
  body += String(lon, 8);
  body += ",";
  body += "\"accuracyMeters\":null,";
  body += "\"batteryPercent\":";
  body += String(battery_percent);
  body += ",";
  body += "\"batteryVoltageMv\":";
  body += String(battery_mv);
  body += ",";
  body += "\"signalStrength\":0,";
  body += "\"satelliteCount\":";
  body += String(satellite_count);
  body += ",";
  body += "\"stepCounter\":";
  body += String(g_steps.steps_total);
  body += ",";
  body += "\"deviceState\":\"";
  body += dev_status;
  body += "\"";
  body += "}";
  return body;
}



// =============================================================================
//  ADC HELPERS
// =============================================================================

/**
 * Read PA6 (ADC_IN6) and return the raw 12-bit value (0–4095).
 * Call adc_init() once in setup() before using these.
 */
uint16_t adc_read_battery_raw() {
  return (uint16_t)analogRead(ADC_PIN);
}

/**
 * Read PA6 and convert to millivolts using the supply voltage as reference.
 */
float adc_read_battery_mv() {
  uint16_t raw = adc_read_battery_raw();
  return (raw / 4095.0f) * VREF_MV;
}

/**
 * Read the internal VREF channel.
 * On STM32G071 the internal reference is nominally 1212 mV (typ).
 * Reading it lets you back-calculate the actual VDDA when it drifts.
 *
 * Actual VDDA (mV) = 1212 * 4095 / vref_raw
 */
uint16_t adc_read_vref_raw() {
  return (uint16_t)analogRead(ADC_VREF_CH);
}

float adc_actual_vdda_mv() {
  uint16_t vref_raw = adc_read_vref_raw();
  if (vref_raw == 0) return 0.0f;
  return (1212.0f * 4095.0f) / (float)vref_raw;
}

/**
 * Read PA6 in mV, corrected using the actual VDDA measured via Vref channel.
 * More accurate than assuming a fixed 3.3 V rail.
 */
float adc_read_battery_mv_calibrated() {
  float vdda_mv = adc_actual_vdda_mv();
  uint16_t raw = adc_read_battery_raw();
  return (raw / 4095.0f) * vdda_mv;
}

/**
 * Call once in setup() — sets resolution and pins to analog input mode.
 */
void adc_init() {
  analogReadResolution(ADC_RESOLUTION);
  pinMode(ADC_PIN, INPUT_ANALOG);
  DEBUG.println("[ADC] Initialised — PA6 + Vref channel ready.");
}

/**
 * Print a full ADC snapshot to the debug console.
 */
void adc_print() {
  uint16_t pa6_raw = adc_read_battery_raw();
  uint16_t vref_raw = adc_read_vref_raw();
  float vdda_mv = adc_actual_vdda_mv();
  float pa6_mv = (pa6_raw / 4095.0f) * vdda_mv;
  g_vdd_mon.bat_volt = pa6_mv / 1000;
  g_vdd_mon.vdd_volt = vdda_mv / 1000;

  g_vdd_mon.bat_volt = g_vdd_mon.bat_volt * 2.0;

  DEBUG.println("[ADC] ---- ADC Snapshot ----");
  DEBUG.print("[ADC] vBAT  raw  : ");
  DEBUG.println(pa6_raw);
  DEBUG.print("[ADC] vBAT  V   : ");
  DEBUG.println(g_vdd_mon.bat_volt, 2);
  DEBUG.print("[ADC] Vref raw  : ");
  DEBUG.println(vref_raw);
  DEBUG.print("[ADC] VDDA V   : ");
  DEBUG.println(g_vdd_mon.vdd_volt, 2);
  DEBUG.println("[ADC] --------------------");

  if (g_vdd_mon.bat_volt > MIN_BAT_VOLT) {
    g_vdd_mon.battery_level = (g_vdd_mon.bat_volt - MIN_BAT_VOLT) * BAT_RATIO;
    if (g_vdd_mon.battery_level > 100) g_vdd_mon.battery_level = 100;
    if (g_vdd_mon.battery_level < 0) g_vdd_mon.battery_level = 0;
  } else g_vdd_mon.battery_level = 0;
}

// ─── Forward declarations (location / sleep) ──────────────────────────────────
void enter_sleep();
void location_timer_tick();


// =============================================================================
//  SLEEP  — LDO off (cuts GSM + GPS) + STM32 RTC deep sleep
// =============================================================================

/**
 * Full system sleep:
 *   1. Graceful GSM power off (AT+CFUN=0)
 *   2. LDO cut — GSM + GPS lose power
 *   3. STM32 enters deep-sleep; RTC wakes it after SLEEP_DURATION_S seconds
 *   4. On wake: re-init serial ports, power on GSM, ready-loop takes over
 */
void enter_sleep() {
#if 0
    DEBUG.print("[SLEEP] Entering deep sleep for ");
    DEBUG.print(SLEEP_DURATION_S);
    DEBUG.println(" s ...");
    delay(50);   // let UART TX flush

    gsm_power_off();   // AT+CFUN=0 then LDO off

    // Reset flags — everything must reinitialise after wake
    g_status.powered_on     = false;
    g_status.gprs_connected = false;
    g_loc.has_fix           = false;
    g_loc.is_stationary     = false;

    // STM32 RTC-based deep sleep
    LowPower.deepSleep(SLEEP_DURATION_S * 1000);

    // ── Resumes here after RTC wakeup ─────────────────────────────────────────
    SerialDBG.begin(DBG_BAUD);
    SerialGSM.begin(GSM_BAUD);
    SerialGPS.begin(GPS_BAUD);
    delay(200);
    DEBUG.println("\r\n[SLEEP] Woke from RTC sleep — reinitialising ...");

    gsm_power_on();   // LDO on + PWRKEY pulse; ready-loop in loop() handles rest
#endif
}

// =============================================================================
//  LOCATION TIMER TICK  — call once per main loop iteration
// =============================================================================

/**
 * Every CHECK_INTERVAL_MS (1 min):
 *   - Compare current GPS position against last stored reference.
 *   - Moved >= MOVE_THRESHOLD_M  → update reference, reset stationary timer.
 *   - Moved <  MOVE_THRESHOLD_M  → accumulate stationary time;
 *                                   if >= STATIONARY_LIMIT_MS → enter_sleep().
 */
void location_timer_tick() {
  unsigned long now = millis();
  if (now - g_loc.last_check_ms < CHECK_INTERVAL_MS) return;
  g_loc.last_check_ms = now;

  if (!gps.location.isValid()) {
    DEBUG.println("[TIMER] No valid GPS fix — skipping location check.");
    return;
  }

  double cur_lat = gps.location.lat();
  double cur_lon = gps.location.lng();

  if (!g_loc.has_fix) {
    DEBUG.println("[TIMER] First GPS fix stored.");
    g_loc.update_reference(cur_lat, cur_lon);
    return;
  }

  // TinyGPSPlus::distanceBetween() uses Haversine and returns metres
  double dist = TinyGPSPlus::distanceBetween(
    g_loc.last_lat, g_loc.last_lon,
    cur_lat, cur_lon);
  DEBUG.print("[TIMER] Distance from last point: ");
  DEBUG.print(dist, 1);
  DEBUG.println(" m");

  if (dist >= MOVE_THRESHOLD_M) {
    DEBUG.println("[TIMER] Movement detected — resetting stationary timer.");
    g_loc.update_reference(cur_lat, cur_lon);
  } else {
    if (!g_loc.is_stationary) {
      g_loc.is_stationary = true;
      g_loc.stationary_since = now;
      DEBUG.println("[TIMER] Device stationary — started stationary timer.");
    }
    unsigned long stat_ms = now - g_loc.stationary_since;
    DEBUG.print("[TIMER] Stationary for ");
    DEBUG.print(stat_ms / 1000UL);
    DEBUG.println(" s");
  }
}

// =============================================================================
//  STM32 HARDWARE TIMER — fires every STEP_TIMER_INTERVAL_MS (1 minute)
//  Calculates steps/min, resets ISR counter, checks inactivity → sleep
// =============================================================================

HardwareTimer *step_timer = nullptr;

// Minute timer callback — mirrors TimerMin() from your original code.
// Atomic snapshot of step_isr_count (noInterrupts guards the 32-bit read on Cortex-M0+).
void oneminute_timer_callback() {
  noInterrupts();
  uint32_t cnt = g_steps.step_isr_count;
  g_steps.step_isr_count = 0;
  wake_count = 0;
  interrupts();

  // ── Steps per minute ──────────────────────────────────────────────────────
  g_steps.steps_this_min = cnt;
  g_steps.steps_total += cnt;
  g_steps.previous_steps = g_steps.steps_total;  // save baseline for next window

  g_steps.minute_counter++;
  if (g_steps.minute_counter > 10000) g_steps.minute_counter = 0;

  // ── Debug heartbeat (matches your TimerMin console output) ────────────────
  INFO.println("+------------------------------+");
  INFO.print("| Minute counter: ");
  INFO.println(g_steps.minute_counter);
  INFO.println("+------------------------------+");

  INFO.print("GSM Status: ");
  DEBUG.println(g_status.gprs_connected ? "Connected" : "Disconnected");
  INFO.print("GPS Status: ");
  DEBUG.println(gps.location.isValid() ? "Valid fix" : "No fix");

  INFO.println("+------------------------------+");
  INFO.print("Steps in last minute: ");
  INFO.println(g_steps.steps_this_min);
  INFO.print("Steps total: ");
  INFO.println(g_steps.steps_total);
  accel_steps_print_status(INFO);
  INFO.println("+------------------------------+");

  // // ── Inactivity → flag sleep (loop() calls enter_sleep() safely) ───────────
  // if (cnt == 0 && g_steps.is_inactive()) {
  //     DEBUG.println("[TIMER] Idle time-out — flagging sleep mode.");
  //     g_steps.sleep_mode_on = true;
  // }

  if (!g_steps.sleep_mode_on) {
    g_steps.sleep_minute_count++;
    DEBUG.print("[TIMER] Idle time: minute ");
    DEBUG.print(g_steps.sleep_minute_count);
    DEBUG.print("/");
    DEBUG.println(IDLE_TIME);
    if (g_steps.sleep_minute_count >= IDLE_TIME) {
      // Time to evaluate
      bool no_steps = (cnt == 0);
      // Compare current position against position stored at sleep entry
      double dist = 0.0;
      bool has_gps = g_status.is_gps_ready;
      if (has_gps) {
        dist = TinyGPSPlus::distanceBetween(
          g_steps.sleep_ref_lat, g_steps.sleep_ref_lon,
          g_loc.current_lat, g_loc.current_lon);
      }
      bool no_movement = !has_gps || (dist < WAKE_MIN_DISTANCE_M);
      DEBUG.print("[TIMER] — in last minute steps: ");
      DEBUG.print(cnt);
      DEBUG.print("  dist from sleep pos: ");
      DEBUG.print(dist, 1);
      DEBUG.println(" m");
      if (no_movement && no_steps) {
        // Neither steps nor meaningful GPS movement since last sleep — sleep again
        DEBUG.println("[TIMER]  flagging sleep.");
        g_steps.sleep_mode_on = true;
        g_steps.send_sleep_data = true;
        g_steps.sleep_minute_count = 0;
        g_steps.start_safe_shut_down = true;
        g_steps.sleep_ref_lat = g_loc.current_lat;
        g_steps.sleep_ref_lon = g_loc.current_lon;
        wake_count = 0;

      } else {
        DEBUG.println("[TIMER] activity detected — updating ref, resetting window.");
        if (has_gps) {
          // Update sleep ref so next window measures from here
          g_steps.sleep_ref_lat = g_loc.current_lat;
          g_steps.sleep_ref_lon = g_loc.current_lon;
        }
      }
    }
  }
}

void oneminute_timer_init() {
  // TIM16 is a free 16-bit timer on STM32G071; HardwareTimer picks a free one automatically
  step_timer = new HardwareTimer(TIM16);
  step_timer->setOverflow(STEP_TIMER_INTERVAL_MS * 1000UL, MICROSEC_FORMAT);
  step_timer->attachInterrupt(oneminute_timer_callback);
  step_timer->resume();
  DEBUG.println("[TIMER] one minute timer started (1 min interval).");
}

// ─── Accelerometer: LIS2DH12 with software step counter ─────────────────────
// The old sensor counted steps in hardware and pulsed INT2 once per step. The
// LIS2DH12 has no pedometer: accel_steps.cpp reads its FIFO from a timer
// interrupt and runs the step counter (libraries/StepwhereMotion). It calls
// this function with the steps it found, in interrupt context, and it does
// what the old INT2 ISR did for each step.
void accel_steps_on_steps(uint32_t steps) {
  g_steps.step_isr_count += steps;
  wake_count += steps;
  if (g_steps.sleep_mode_on && wake_count >= MIN_STEPS) {
    g_steps.sleep_mode_on = false;
    g_steps.send_sleep_data = false;
    g_steps.sleep_minute_count = 0;
    wake_count = 0;
    // loop() sees sleep_mode_on = false and skips enter_sleep()
  }
  g_steps.last_active_ms = millis();
  g_steps.active = true;
}

void lis2dh_init() {
  if (accel_steps_begin(dev_i2c, ACC_SA0)) {
    DEBUG.println("[ACCEL] LIS2DH12 found at 0x19, step counter running.");
  } else {
    ERR.println("[ACCEL] LIS2DH12 not found !!! (retrying every 5 s)");
  }
}

//=============================================================================

ChargeStatus getChargeStatusRaw() {
  bool chrg = digitalRead(CHRG_PIN);
  bool stdby = digitalRead(STDBY_PIN);

  /*
     * TP4056 outputs are open-drain and active LOW.
     *
     * CHRG  = LOW  -> charging
     * STDBY = LOW  -> charging complete
     *
     * HIGH means high impedance, provided the pull-up is present.
     */

  if (chrg == LOW && stdby == HIGH) {
    return CHARGE_CHARGING;
  }

  if (chrg == HIGH && stdby == LOW) {
    return CHARGE_COMPLETE;
  }

  if (chrg == HIGH && stdby == HIGH) {
    return CHARGE_IDLE;
  }

  return CHARGE_ERROR;
}

ChargeStatus getChargeStatus() {
  static ChargeStatus lastStable = CHARGE_ERROR;
  static ChargeStatus lastRaw = CHARGE_ERROR;
  static unsigned long lastChangeTime = 0;

  ChargeStatus raw = getChargeStatusRaw();

  if (raw != lastRaw) {
    lastRaw = raw;
    lastChangeTime = millis();
  }

  if (millis() - lastChangeTime > 50) {  // 50ms debounce
    lastStable = raw;
  }
  return lastStable;
}

void printChargeStatus() {
  static ChargeStatus previousStatus = CHARGE_UNKNOWN;
  ChargeStatus status = getChargeStatus();
    // Nothing changed
    if (status == previousStatus)
      return;
    previousStatus = status;


  switch (status) {

    case CHARGE_CHARGING:
      {
        is_charging = true;
        DEBUG.println("[Battery] : CHARGING");
        break;
      }

    case CHARGE_COMPLETE:
      {
        is_charging = false;
        DEBUG.println("[Battery] : CHARGE COMPLETE");
        break;
      }
    case CHARGE_IDLE:
      {
        is_charging = false;
        DEBUG.println("[Battery] : IDLE / NO CHARGE");
        break;
      }
    case CHARGE_ERROR:
      is_charging = false;
      DEBUG.println("[Battery] : ERROR / INVALID STATUS");
      break;

    default:
      DEBUG.println("[Battery] : UNKNOWN");
      break;
  }
}

bool battery_powerProtection() {
  float batteryVoltage = g_vdd_mon.bat_volt;
  static bool prev_bat_low_status = false;
  bool bat_low_status = false;

  /*
     * Battery critically low.
     */
  if (batteryVoltage < BATTERY_LOW_OFF) {
    bat_low_status = true;
  }
  /*
     * Battery is OK.
     */
  else if (batteryVoltage >= BATTERY_LOW_ON) {
    bat_low_status = false;
  }
  // Nothing changed
  DEBUG.println(bat_low_status ? "[BATTERY] : LOW " : "[BATTERY] : OK");
  if (bat_low_status == prev_bat_low_status)
    return bat_low_status;

  if (bat_low_status)
    ldo_enable(false);

  prev_bat_low_status = bat_low_status;
  return bat_low_status;
}

// No Charging Thing is available
volatile bool chargeStatusChanged = false;

void chargeStatusISR() {
  chargeStatusChanged = true;
}

/*
void BMS_ISR() {
  if (digitalRead(CHRG_PIN) == LOW) {
    DEBUG.println("ISR Charging On");
    DEBUG.println("System Reboot...");
    delay(5);
    HAL_NVIC_SystemReset();
  } else {
    DEBUG.println("ISR Charging Off");
    DEBUG.println("System Reboot...");
    delay(5);
    HAL_NVIC_SystemReset();
  }
  delay(50);
}
*/

//=============================================================================


// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  pinMode(GSM_PWRKEY, OUTPUT);
  digitalWrite(GSM_PWRKEY, HIGH);  // PWRKEY idle high

  pinMode(LDO_EN, OUTPUT);
  digitalWrite(LDO_EN, LOW);  // LDO off initially

  // Initialise ADC
  adc_init();
  //========================================================================
  SerialDBG.begin(DBG_BAUD);
  delay(200);
  INFO.println("[SETUP]: === STM32G071CB — N706 GSM / G7A GPS Tracker ===");
  if (gpsreset_magic == GPS_RESET_MAGIC) {
    /*
         * Clear immediately.
         */
    gpsreset_magic = 0;
    DEBUG.println("System reset reason : GPS");
  }
  //========================================================================
  pinMode(CHRG_PIN, INPUT_PULLUP);
  pinMode(STDBY_PIN, INPUT_PULLUP);
  printChargeStatus();
  adc_print();

  //========================================================================
  ldo_enable(true);
  delay(500);  // LDO settle
  SerialGSM.begin(GSM_BAUD);
  SerialGPS.begin(GPS_BAUD);

  // Initialise RTC-based low power (required before LowPower.deepSleep)
  // LowPower.begin();

  lis2dh_init();
  g_steps.last_active_ms = millis();
  oneminute_timer_init();

  gsm_power_on();
  // ── Retry loop: repeat every failed step until all flags are set ──────────
  uint8_t count = 0;
  while (!g_status.ready() && count < 5) {
    INFO.println("[SETUP] System not ready — running init sequence ...");
    count++;
    // Step 1: modem power + SIM check
    if (!g_status.powered_on) {
      if (modem_check()) {
        g_status.powered_on = true;
        INFO.println("[SETUP] Modem OK.");
      } else {
        DEBUG.println("[SETUP] Modem check failed — power cycling ...");
        gsm_power_off();
        delay(2000);
        gsm_power_on();
        delay(3000);
        g_status.print();
        continue;  // restart from top of while
      }
    }
    // Step 2: IMEI
    if (!g_status.imei_valid) {
      g_imei = get_imei();
      g_status.imei_valid = (g_imei.length() == 15);
      if (!g_status.imei_valid) {
        ERR.println("[SETUP] IMEI invalid — retrying in 3 s ...");
        delay(3000);
        g_status.print();
        continue;
      }
      INFO.println("[SETUP] IMEI OK.");
    }

    // Step 3: GPRS
    if (!g_status.gprs_connected) {
      g_status.gprs_connected = gprs_on();
      if (!g_status.gprs_connected) {
        DEBUG.println("[SETUP] GPRS failed — retrying in 5 s ...");
        delay(5000);
        g_status.print();
        continue;
      }
      INFO.println("[SETUP] GPRS OK.");
    }
  }
  //  if(!cert_upload_ca()){
  //     DEBUG.println("[SETUP] WARNING: CA certificate not available — HTTPS will fail.");
  //   }else if (!https_configure_ssl()) {
  //       DEBUG.println("[SETUP] WARNING: SSL config failed — HTTPS will fail.");
  //   }

  g_status.print();
  INFO.println("[SETUP] Init complete.\r\n");
  INFO.println(APP_VERSION);
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP  — collect GPS for 25 s, then POST
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
  printChargeStatus();
  adc_print();
  // if (is_charging && (!g_steps.send_sleep_data)) {
  //   DEBUG.println("[BATTERY] : CHARGE ON");
  //   delay(5000);
  //   if (g_status.powered_on == true) {
  //     INFO.println("[MAIN] Shutdown (GSM OFF)");
  //     gsm_power_off();
  //     delay(2000);
  //     g_status.reset();
  //   }
  //   return;
  // }

  if (battery_powerProtection() && (!g_steps.send_sleep_data)) {
    if (g_status.powered_on == true) {
      INFO.println("[MAIN] Shutdown ldo (GSM /GPS OFF)");
     // gsm_power_off();
      ldo_enable(0);
      delay(2000);
      g_status.reset();
    }
    delay(5000);
    return;
  }

  if (!g_steps.sleep_mode_on || g_steps.send_sleep_data) {
    // If GPRS dropped at runtime, clear the flag and re-run the ready loop
    if (!g_status.ready()) {
      DEBUG.println("[MAIN] System not ready — re-running init sequence ...");

      if (!g_status.ready()) {
        if (!g_status.powered_on) {
          if (modem_check()) {
            g_status.powered_on = true;

          } else {
            ERR.println("[MAIN] Modem lost — power cycling ...");
            gsm_power_off();
            delay(2000);
            gsm_power_on();
            delay(3000);
            g_status.print();
            return;
          }
        }

        // IMEI is read once in setup() and never re-fetched at runtime
        if (!g_status.imei_valid) {
          g_imei = get_imei();
          g_status.imei_valid = (g_imei.length() == 15);
          if (!g_status.imei_valid) {
            ERR.println("[SETUP] IMEI invalid — retrying in 3 s ...");
            delay(3000);
            g_status.print();
            return;
          }
          INFO.println("[SETUP] IMEI OK.");
        }

        if (!g_status.gprs_connected) {
          DEBUG.println("[MAIN] GPRS down — retrying ...");
          g_status.gprs_connected = gprs_on();
          if (!g_status.gprs_connected) {
            delay(5000);
            g_status.print();
            return;
          }
        }
      }
      g_status.print();
      INFO.println("[MAIN] System ready — resuming normal operation.");
    }

    gps_feed(1000);
    gps_check_timeout();
    print_gps_info();
    // location_timer_tick();
    // adc_print();

    // char json[256];
    // build_json(json, sizeof(json), g_imei);
    // INFO.print("[MAIN] Payload: ");
    // INFO.println(json);
    // String http_payload = "latitude=" + String(current_lat, 8) + "&longitude=" + String(current_lng, 8) + "&device_name=" + device_id + "&location=" + String(bat_volt, 2) + "V," + String(battery_level) + "%," + String(stepCountTotal) + "," + String(num_sat);  //+ String(battery_level) + "%,"
    // DEBUG.println(http_payload);

    if (millis() - g_http_status.last_send > HTTP_REQ_DELAY) {
      if (g_status.ready()) {
        // current_lat = 28.61393912;
        // current_lng = 77.20902313;
        // String http_payload = "latitude=" + String( g_loc.current_lat, 8) + "&longitude=" + String( g_loc.current_lon, 8) + "&device_name=" + g_imei + "&location=" + String(g_vdd_mon.bat_volt, 2) + "V," + String(g_vdd_mon.battery_level) + "%," + String(g_steps.steps_total) + "," + String( g_loc.num_satellite);//+","+ String(g_steps.sleep_mode_on);// + "%,"
        // INFO.println(http_payload);
        // static uint8_t reconnect = 0;
        // bool ok = http_post(http_payload.c_str());
        // bool ok = https_post_v1(http_payload.c_str());
        // bool ok = neoway_test(NEOWAY_DATA);
        bool is_sleep_on = g_steps.sleep_mode_on;
        String telemetry_payload = build_location_telemetry_json();
        INFO.print("[MAIN] HTTP ingest JSON payload: ");
        INFO.println(telemetry_payload);
       
        bool ok = http_post_json(telemetry_payload.c_str());
        if (!ok) {
          g_http_status.reconnect_cnt++;
          if (g_http_status.reconnect_cnt >= 5) {
            g_http_status.reconnect_cnt = 0;
            DEBUG.println("[MAIN] HTTP failed — marking GPRS for reconnect.");
            g_status.gprs_connected = false;
          }
        } else {
          if(is_sleep_on){
            if (g_steps.sleep_mode_on && g_steps.send_sleep_data)
              g_steps.send_sleep_data = false;
          }
        }
        g_http_status.last_send = millis();
      }
    }
  } else {
    if (g_steps.start_safe_shut_down && !g_steps.send_sleep_data) {
      // ldo_enable(0);
      INFO.println("[MAIN] Shutdown (GSM OFF)");
      gsm_power_off();
      delay(2000);
      g_status.reset();
      // g_steps.sleep_mode_on = false;
      g_steps.send_sleep_data = false;
      g_steps.start_safe_shut_down = false;
    }
    gps_feed(1000);
    gps_check_timeout();
    __WFI();
  }
  delay(1000);
}
