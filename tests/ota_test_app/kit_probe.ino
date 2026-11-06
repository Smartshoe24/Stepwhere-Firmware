/*
 * kit_probe.ino - Neoway N706B probe built into the OTA test app.
 *   'n' runs the probe (power on, network, three download methods)
 *   'm' AT pass-through (~~~ to leave)
 * Same code as tests/modem_probe, renamed to avoid clashes.
 */
#define GSM_PWRKEY PB13
#define LDO_EN     PA10
#define APN        "internet"
/* Download target, changeable at run time with:  p <host> <port> <path>  */
static char g_host[64] = "httpbin.org";
static char g_path[64] = "/range/600";
static unsigned g_port = 80;
static char g_req[256];
static size_t g_req_len;

static void build_request()
{
    g_req_len = (size_t)snprintf(g_req, sizeof g_req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=100-199\r\nUser-Agent: stepwhere-probe\r\nConnection: close\r\n\r\n",
        g_path, g_host);
}

/* "p host port path" from the debug UART */
void kit_set_target()
{
    char line[160];
    size_t n = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 3000 && n < sizeof line - 1) {
        wdg_kick();
        if (!SerialDBG.available()) continue;
        char c = (char)SerialDBG.read();
        if (c == '\r' || c == '\n') { if (n) break; else continue; }
        line[n++] = c;
    }
    line[n] = 0;
    char h[64], pa[64];
    unsigned po;
    if (sscanf(line, " %63s %u %63s", h, &po, pa) == 3) {
        strcpy(g_host, h); strcpy(g_path, pa); g_port = po;
        SerialDBG.print("[probe] target set: http://");
    } else {
        SerialDBG.print("[probe] usage: p <host> <port> <path>   current: http://");
    }
    SerialDBG.print(g_host); SerialDBG.print(':'); SerialDBG.print(g_port); SerialDBG.println(g_path);
}

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
         pr_at("AT+XIIC?", 3000, "OK");
        const char *p = strstr(g_pcap, "+XIIC:");
        if (p && strchr(p, '1') && strchr(p, '.')) return true;
        delay(2000);
    }
    SerialDBG.println("[probe] no IP address from AT+XIIC");
    return false;
}

static void pr_url(bool scheme)
{
    char cmd[200];
    snprintf(cmd, sizeof cmd, "AT+HTTPPARA=url,\"%s%s%s\"", scheme ? "http://" : "", g_host, g_path);
    pr_at(cmd, 3000);
    snprintf(cmd, sizeof cmd, "AT+HTTPPARA=port,%u", g_port);
    pr_at(cmd, 3000);
}

static void pr_method_a(bool scheme)
{
    SerialDBG.println();
    SerialDBG.print("=== Method A");
    SerialDBG.print(scheme ? "1" : "2");
    SerialDBG.println(scheme ? ": HTTPACTION=0, url with http:// ===" : ": HTTPACTION=0, url without http:// ===");
    pr_url(scheme);
    pr_at("AT+HTTPSETUP", 20000);
    pr_at("AT+HTTPACTION=0", 25000, "+HTTPCLOSED", 5000);
    pr_capture(3000, NULL, 1500);          /* anything arriving after CLOSED */
    if (g_pcap_len) pr_dump(g_pcap, g_pcap_len);
    pr_at("AT+HTTPCLOSE", 3000);
}

static void pr_method_b()
{
    char cmd[40];
    SerialDBG.println();
    SerialDBG.println("=== Method B: HTTPACTION=99 (own request with Range header) ===");
    pr_url(true);
    pr_at("AT+HTTPSETUP", 20000);
    snprintf(cmd, sizeof cmd, "AT+HTTPACTION=99,%u", (unsigned)g_req_len);
    if (pr_at(cmd, 5000, ">", 0)) {
        SerialDBG.println("  >> (raw request with Range: bytes=100-199)");
        SerialGSM.print(g_req);
        pr_capture(25000, "+HTTPCLOSED", 5000);
        pr_dump(g_pcap, g_pcap_len);
        pr_capture(3000, NULL, 1500);
        if (g_pcap_len) pr_dump(g_pcap, g_pcap_len);
    }
    pr_at("AT+HTTPCLOSE", 3000);
}

static void pr_method_c()
{
    char cmd[120];
    SerialDBG.println();
    SerialDBG.println("=== Method C: raw TCP socket (TCPSETUP / TCPSEND) ===");
    snprintf(cmd, sizeof cmd, "AT+TCPSETUP=0,%s,%u", g_host, g_port);
    /* wait for the WHOLE result line, e.g. "+TCPSETUP: 0,OK" or "0,FAIL" */
    pr_at(cmd, 30000, "+TCPSETUP: 0,", 0);
    pr_capture(3000, "\n", 0);
    pr_dump(g_pcap, g_pcap_len);
    snprintf(cmd, sizeof cmd, "AT+TCPSEND=0,%u", (unsigned)g_req_len);
    if (pr_at(cmd, 5000, ">", 0)) {
        SerialDBG.println("  >> (raw request with Range: bytes=100-199)");
        SerialGSM.print(g_req);
        pr_capture(25000, "+TCPCLOSE", 5000);
        pr_dump(g_pcap, g_pcap_len);
    }
    pr_at("AT+TCPCLOSE=0", 3000);
}

static void pr_dns()
{
    char cmd[100];
    SerialDBG.println();
    SerialDBG.println("=== DNS lookup ===");
    snprintf(cmd, sizeof cmd, "AT+DNS=\"%s\"", g_host);
    pr_at(cmd, 15000, "+DNS", 2000);
}

void kit_probe()
{
    SerialDBG.println();
    SerialDBG.println("################ N706B PROBE START ################");
    pr_power_on();
    if (pr_network_up()) {
        build_request();
        SerialDBG.print("[probe] target http://");
        SerialDBG.print(g_host); SerialDBG.print(':'); SerialDBG.print(g_port); SerialDBG.println(g_path);
        pr_dns();
        pr_method_a(true);
        pr_method_a(false);
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

