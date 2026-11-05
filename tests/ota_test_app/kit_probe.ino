/*
 * kit_probe.ino - Neoway N706B probe built into the OTA test app.
 *   'n' runs the probe (power on, network, three download methods)
 *   'm' AT pass-through (~~~ to leave)
 * Same code as tests/modem_probe, renamed to avoid clashes.
 */
#define GSM_PWRKEY PB13
#define LDO_EN     PA10
#define APN        "internet"
#define TEST_HOST  "httpbin.org"
#define TEST_PATH  "/range/600"

HardwareSerial SerialGSM(PA3, PA2);   // USART2 N706B
static char g_pcap[6144];
static size_t g_pcap_len;

void kit_probe_init()
{
    pinMode(LDO_EN, OUTPUT);
    digitalWrite(LDO_EN, LOW);
    pinMode(GSM_PWRKEY, OUTPUT);
    digitalWrite(GSM_PWRKEY, HIGH);
    SerialGSM.begin(115200);
}

/* ------------------------------------------------------------------------ */

static void pr_dump(const char *buf, size_t n)
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
static bool pr_capture(uint32_t timeout_ms, const char *until, uint32_t quiet_ms = 0)
{
    uint32_t t0 = millis(), last = millis();
    g_pcap_len = 0;
    g_pcap[0] = '\0';
    while ((uint32_t)(millis() - t0) < timeout_ms) {
        wdg_kick();
        while (SerialGSM.available()) {
            char c = (char)SerialGSM.read();
            if (g_pcap_len < sizeof g_pcap - 1) {
                g_pcap[g_pcap_len++] = c;
                g_pcap[g_pcap_len] = '\0';
            }
            last = millis();
        }
        if (until && strstr(g_pcap, until)) return true;
        if (quiet_ms && g_pcap_len && (uint32_t)(millis() - last) > quiet_ms) return until == NULL;
    }
    return until == NULL;
}

static bool  pr_at(const char *cmd, uint32_t timeout_ms, const char *until = "OK", uint32_t quiet_ms = 300)
{
    while (SerialGSM.available()) SerialGSM.read();
    SerialDBG.print("  >> ");
    SerialDBG.println(cmd);
    SerialGSM.print(cmd);
    SerialGSM.print("\r");
    uint32_t t0 = millis();
    bool ok = pr_capture(timeout_ms, until, quiet_ms);
    /* "ERROR" ends a command early */
    if (!ok && strstr(g_pcap, "ERROR")) ok = false;
    pr_dump(g_pcap, g_pcap_len);
    SerialDBG.print("    (");
    SerialDBG.print(millis() - t0);
    SerialDBG.print(" ms, ");
    SerialDBG.println(ok ? "matched)" : "NOT matched)");
    return ok;
}

/* ------------------------------------------------------------------------ */

static void pr_power_on()
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
    pr_capture(5000, NULL);
    if (g_pcap_len) pr_dump(g_pcap, g_pcap_len);
}

static bool pr_network_up()
{
    bool alive = false;
    for (int i = 0; i < 10 && !alive; i++) {
        alive =  pr_at("AT", 1000);
        if (!alive) delay(1000);
    }
    if (!alive) {
        SerialDBG.println("[probe] modem does not answer AT. Check power and UART2 wiring.");
        return false;
    }
     pr_at("ATE0", 1000);
     pr_at("ATI", 2000);
     pr_at("AT+CGMR", 2000);
    for (int i = 0; i < 10; i++) {
        if (pr_at("AT+CPIN?", 2000, "READY")) break;
        delay(1000);
    }
     pr_at("AT+CSQ", 2000);
    for (int i = 0; i < 30; i++) {           /* up to ~60 s to register on LTE */
         pr_at("AT+CEREG?", 2000);
        if (strstr(g_pcap, ",1") || strstr(g_pcap, ",5")) break;
        delay(1500);
    }
     pr_at("AT+CGDCONT=1,\"IP\",\"" APN "\"", 3000);
     pr_at("AT+XIIC=0", 3000);
     pr_at("AT+XIIC=1", 20000);
    for (int i = 0; i < 15; i++) {
         pr_at("AT+XIIC?", 3000, "+XIIC:");
        const char *p = strstr(g_pcap, "+XIIC:");
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

static void pr_method_a()
{
    SerialDBG.println();
    SerialDBG.println("=== Method A: AT+HTTPACTION=0 (plain GET, whole file) ===");
     pr_at("AT+HTTPPARA=url,\"http://" TEST_HOST TEST_PATH "\"", 3000);
     pr_at("AT+HTTPPARA=port,80", 3000);
     pr_at("AT+HTTPSETUP", 20000);
     pr_at("AT+HTTPACTION=0", 20000, "+HTTPCLOSED", 3000);
     pr_at("AT+HTTPCLOSE", 3000);
}

static void pr_method_b()
{
    char cmd[40];
    SerialDBG.println();
    SerialDBG.println("=== Method B: AT+HTTPACTION=99 (custom request with Range header) ===");
     pr_at("AT+HTTPPARA=url,\"http://" TEST_HOST TEST_PATH "\"", 3000);
     pr_at("AT+HTTPPARA=port,80", 3000);
     pr_at("AT+HTTPSETUP", 20000);
    snprintf(cmd, sizeof cmd, "AT+HTTPACTION=99,%u", (unsigned)(sizeof RAW_REQUEST - 1));
    if (pr_at(cmd, 5000, ">", 0)) {
        SerialDBG.println("  >> (raw request with Range: bytes=100-199)");
        SerialGSM.print(RAW_REQUEST);
        pr_capture(20000, "+HTTPCLOSED", 3000);
        pr_dump(g_pcap, g_pcap_len);
    }
     pr_at("AT+HTTPCLOSE", 3000);
}

static void pr_method_c()
{
    char cmd[64];
    SerialDBG.println();
    SerialDBG.println("=== Method C: raw TCP socket (AT+TCPSETUP / AT+TCPSEND) ===");
     pr_at("AT+TCPSETUP=0," TEST_HOST ",80", 20000, "+TCPSETUP:", 0);
    snprintf(cmd, sizeof cmd, "AT+TCPSEND=0,%u", (unsigned)(sizeof RAW_REQUEST - 1));
    if (pr_at(cmd, 5000, ">", 0)) {
        SerialDBG.println("  >> (raw request with Range: bytes=100-199)");
        SerialGSM.print(RAW_REQUEST);
        pr_capture(20000, "+TCPCLOSE", 3000);
        pr_dump(g_pcap, g_pcap_len);
    }
     pr_at("AT+TCPCLOSE=0", 3000);
}

void kit_probe()
{
    SerialDBG.println();
    SerialDBG.println("################ N706B PROBE START ################");
    pr_power_on();
    if (pr_network_up()) {
        pr_method_a();
        pr_method_b();
        pr_method_c();
    }
    SerialDBG.println("################ N706B PROBE END ##################");
    SerialDBG.println("Copy everything between START and END and send it.");
}

/* ------------------------------------------------------------------------ */

void kit_passthrough()
{
    int tildes = 0;
    SerialDBG.println("[AT pass-through] type AT commands, Enter sends. Type ~~~ to leave.");
    for (;;) {
        wdg_kick();
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

