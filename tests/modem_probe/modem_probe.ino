/*
 * modem_probe.ino - Finds out how the Neoway N706B downloads data, before the
 * OTA downloader is written. Flash at 0x08000000 (no bootloader needed).
 *
 * Debug UART: USART1, PB6 TX / PB7 RX, 115200. Commands:
 *   n   run the full probe (power on, network, three download methods)
 *   m   AT pass-through: type AT commands to the modem; type ~~~ to leave
 *   h   help
 *
 * Downloads use http://httpbin.org/range/600, a public test URL that returns
 * 600 bytes "abcd...xyzabcd..." and supports byte ranges, like CloudFront.
 * All modem replies are captured to RAM first and printed afterwards, so the
 * 64-byte UART buffer cannot overflow while data arrives.
 */
#include <HardwareSerial.h>

#define GSM_PWRKEY PB13
#define LDO_EN     PA10
#define APN        "internet"

#define TEST_HOST  "httpbin.org"
#define TEST_PATH  "/range/600"

HardwareSerial SerialDBG(PB7, PB6);   // USART1 debug
HardwareSerial SerialGSM(PA3, PA2);   // USART2 N706B

static char g_cap[6144];
static size_t g_cap_len;

/* ------------------------------------------------------------------------ */

static void dump(const char *buf, size_t n)
{
    size_t shown = n > 2500 ? 2500 : n;
    SerialDBG.print("    << ");
    for (size_t i = 0; i < shown; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') { SerialDBG.print("\\n\n       "); }
        else if (c == '\r') { SerialDBG.print("\\r"); }
        else if (c >= 32 && c < 127) { SerialDBG.write(c); }
        else {
            static const char hx[] = "0123456789ABCDEF";
            SerialDBG.print("\\x");
            SerialDBG.write(hx[c >> 4]);
            SerialDBG.write(hx[c & 15]);
        }
    }
    SerialDBG.println();
    if (shown < n) {
        SerialDBG.print("    ... (");
        SerialDBG.print(n - shown);
        SerialDBG.println(" more bytes not shown)");
    }
    SerialDBG.print("    [");
    SerialDBG.print(n);
    SerialDBG.println(" bytes captured]");
}

/* Capture modem output until `until` appears (if not NULL) or timeout.
 * `quiet_ms` > 0: also stop after this long with no new bytes, once
 * something has arrived. */
static bool capture(uint32_t timeout_ms, const char *until, uint32_t quiet_ms = 0)
{
    uint32_t t0 = millis(), last = millis();
    g_cap_len = 0;
    g_cap[0] = '\0';
    while ((uint32_t)(millis() - t0) < timeout_ms) {
        while (SerialGSM.available()) {
            char c = (char)SerialGSM.read();
            if (g_cap_len < sizeof g_cap - 1) {
                g_cap[g_cap_len++] = c;
                g_cap[g_cap_len] = '\0';
            }
            last = millis();
        }
        if (until && strstr(g_cap, until)) return true;
        if (quiet_ms && g_cap_len && (uint32_t)(millis() - last) > quiet_ms) return until == NULL;
    }
    return until == NULL;
}

static bool at(const char *cmd, uint32_t timeout_ms, const char *until = "OK", uint32_t quiet_ms = 300)
{
    while (SerialGSM.available()) SerialGSM.read();
    SerialDBG.print("  >> ");
    SerialDBG.println(cmd);
    SerialGSM.print(cmd);
    SerialGSM.print("\r");
    uint32_t t0 = millis();
    bool ok = capture(timeout_ms, until, quiet_ms);
    /* "ERROR" ends a command early */
    if (!ok && strstr(g_cap, "ERROR")) ok = false;
    dump(g_cap, g_cap_len);
    SerialDBG.print("    (");
    SerialDBG.print(millis() - t0);
    SerialDBG.print(" ms, ");
    SerialDBG.println(ok ? "matched)" : "NOT matched)");
    return ok;
}

/* ------------------------------------------------------------------------ */

static void modem_power_on()
{
    SerialDBG.println("[probe] LDO on, PWRKEY pulse (same sequence as stepwhere.ino)");
    digitalWrite(LDO_EN, HIGH);
    delay(600);
    digitalWrite(GSM_PWRKEY, HIGH);
    delay(100);
    digitalWrite(GSM_PWRKEY, LOW);
    delay(1200);
    digitalWrite(GSM_PWRKEY, HIGH);
    SerialDBG.println("[probe] waiting for modem boot (up to 15 s)");
    capture(5000, NULL);
    if (g_cap_len) dump(g_cap, g_cap_len);
}

static bool network_up()
{
    bool alive = false;
    for (int i = 0; i < 10 && !alive; i++) {
        alive = at("AT", 1000);
        if (!alive) delay(1000);
    }
    if (!alive) {
        SerialDBG.println("[probe] modem does not answer AT. Check power and UART2 wiring.");
        return false;
    }
    at("ATE0", 1000);
    at("ATI", 2000);
    at("AT+CGMR", 2000);
    for (int i = 0; i < 10; i++) {
        if (at("AT+CPIN?", 2000, "READY")) break;
        delay(1000);
    }
    at("AT+CSQ", 2000);
    for (int i = 0; i < 30; i++) {           /* up to ~60 s to register on LTE */
        at("AT+CEREG?", 2000);
        if (strstr(g_cap, ",1") || strstr(g_cap, ",5")) break;
        delay(1500);
    }
    at("AT+CGDCONT=1,\"IP\",\"" APN "\"", 3000);
    at("AT+XIIC=0", 3000);
    at("AT+XIIC=1", 20000);
    for (int i = 0; i < 15; i++) {
        at("AT+XIIC?", 3000, "+XIIC:");
        const char *p = strstr(g_cap, "+XIIC:");
        if (p && strchr(p, '1') && strchr(p, '.')) return true;
        delay(2000);
    }
    SerialDBG.println("[probe] no IP address from AT+XIIC");
    return false;
}

static const char RAW_REQUEST[] =
    "GET " TEST_PATH " HTTP/1.1\r\n"
    "Host: " TEST_HOST "\r\n"
    "Range: bytes=100-199\r\n"
    "Connection: close\r\n"
    "\r\n";

static void method_a_http_get()
{
    SerialDBG.println();
    SerialDBG.println("=== Method A: AT+HTTPACTION=0 (plain GET, whole file) ===");
    at("AT+HTTPPARA=url,\"http://" TEST_HOST TEST_PATH "\"", 3000);
    at("AT+HTTPPARA=port,80", 3000);
    at("AT+HTTPSETUP", 20000);
    at("AT+HTTPACTION=0", 20000, "+HTTPCLOSED", 3000);
    at("AT+HTTPCLOSE", 3000);
}

static void method_b_http_custom()
{
    char cmd[40];
    SerialDBG.println();
    SerialDBG.println("=== Method B: AT+HTTPACTION=99 (custom request with Range header) ===");
    at("AT+HTTPPARA=url,\"http://" TEST_HOST TEST_PATH "\"", 3000);
    at("AT+HTTPPARA=port,80", 3000);
    at("AT+HTTPSETUP", 20000);
    snprintf(cmd, sizeof cmd, "AT+HTTPACTION=99,%u", (unsigned)(sizeof RAW_REQUEST - 1));
    if (at(cmd, 5000, ">", 0)) {
        SerialDBG.println("  >> (raw request with Range: bytes=100-199)");
        SerialGSM.print(RAW_REQUEST);
        capture(20000, "+HTTPCLOSED", 3000);
        dump(g_cap, g_cap_len);
    }
    at("AT+HTTPCLOSE", 3000);
}

static void method_c_tcp()
{
    char cmd[64];
    SerialDBG.println();
    SerialDBG.println("=== Method C: raw TCP socket (AT+TCPSETUP / AT+TCPSEND) ===");
    at("AT+TCPSETUP=0," TEST_HOST ",80", 20000, "+TCPSETUP:", 0);
    snprintf(cmd, sizeof cmd, "AT+TCPSEND=0,%u", (unsigned)(sizeof RAW_REQUEST - 1));
    if (at(cmd, 5000, ">", 0)) {
        SerialDBG.println("  >> (raw request with Range: bytes=100-199)");
        SerialGSM.print(RAW_REQUEST);
        capture(20000, "+TCPCLOSE", 3000);
        dump(g_cap, g_cap_len);
    }
    at("AT+TCPCLOSE=0", 3000);
}

static void run_probe()
{
    SerialDBG.println();
    SerialDBG.println("################ N706B PROBE START ################");
    modem_power_on();
    if (network_up()) {
        method_a_http_get();
        method_b_http_custom();
        method_c_tcp();
    }
    SerialDBG.println("################ N706B PROBE END ##################");
    SerialDBG.println("Copy everything between START and END and send it.");
}

/* ------------------------------------------------------------------------ */

static void passthrough()
{
    int tildes = 0;
    SerialDBG.println("[AT pass-through] type AT commands, Enter sends. Type ~~~ to leave.");
    for (;;) {
        while (SerialGSM.available()) SerialDBG.write(SerialGSM.read());
        if (!SerialDBG.available()) continue;
        int c = SerialDBG.read();
        if (c == '~') {
            if (++tildes == 3) break;
            continue;
        }
        tildes = 0;
        if (c == '\n') continue;           /* Enter may send \r\n or \n */
        if (c == '\r') { SerialGSM.write('\r'); SerialDBG.println(); continue; }
        SerialGSM.write((uint8_t)c);
        SerialDBG.write((uint8_t)c);       /* local echo */
    }
    SerialDBG.println();
    SerialDBG.println("[AT pass-through] left");
}

static void help()
{
    SerialDBG.println("Commands: n = run N706B probe, m = AT pass-through (~~~ to leave), h = help");
}

void setup()
{
    pinMode(LDO_EN, OUTPUT);
    digitalWrite(LDO_EN, LOW);
    pinMode(GSM_PWRKEY, OUTPUT);
    digitalWrite(GSM_PWRKEY, HIGH);
    SerialDBG.begin(115200);
    SerialGSM.begin(115200);
    delay(200);
    SerialDBG.println();
    SerialDBG.println("=== Stepwhere N706B modem probe ===");
    help();
}

void loop()
{
    if (!SerialDBG.available()) return;
    switch (SerialDBG.read()) {
    case 'n': case 'N': run_probe(); help(); break;
    case 'm': case 'M': passthrough(); help(); break;
    case 'h': case 'H': case '?': help(); break;
    default: break;
    }
}
