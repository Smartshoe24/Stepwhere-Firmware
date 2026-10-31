/*
 * modem_power_test.ino - Rev11 bring-up: why does the N706B modem not answer AT?
 *
 * Flash at 0x08000000. Debug UART USART1 PB6 TX / PB7 RX, 115200.
 *
 * Key t runs the full automatic test (about 2 minutes) and prints a summary:
 *   1. modem power off, check the modem RX line (PA3) with power off
 *   2. PA10 HIGH (modem + GPS power switch on), check PA3 again
 *   3. power key method A = what firmware v1.2 does (PB13 idle HIGH, LOW 1.2 s)
 *      -> listen 15 s for boot messages, send AT, scan baud rates + swapped TX/RX
 *   4. power off 5 s
 *   5. power key method B = the opposite polarity (PB13 idle LOW, HIGH 1.2 s)
 *      -> same checks
 *   6. GPS: listen 5 s on USART3 (PB9 RX / PB8 TX, 9600)
 * During steps 3 and 5 the battery voltage (PB10) is sampled to catch dips.
 *
 * Manual keys: l = toggle PA10, k = toggle PB13, a = send AT (115200),
 * w = listen 15 s, g = GPS listen 5 s, m = AT pass-through (~~~ leaves),
 * i = pin states, r = reset, h = help.
 *
 * Pin assumptions (from stepwhere.ino): PA10 = GSM/GPS power enable,
 * PB13 = PWRKEY (polarity unknown on Rev11, both are tried),
 * USART2 PA2 TX / PA3 RX = N706B, 115200.
 */
#include <HardwareSerial.h>

#define VERSION    "modem_power_test v0.1"
#define LDO_EN     PA10
#define GSM_PWRKEY PB13
#define GSM_TX_PIN PA2
#define GSM_RX_PIN PA3
#define BAT_PIN    PB10

HardwareSerial SerialDBG(PB7, PB6);  // USART1 debug
HardwareSerial SerialGSM(PA3, PA2);  // USART2 N706B (rx, tx)
HardwareSerial SerialGPS(PB9, PB8);  // USART3 G7A (rx, tx)

static uint32_t g_baud = 115200;
static bool g_swap = false;
static bool g_ldo = false;
static bool g_key = false;

/* result flags for the summary */
static String g_line_off, g_line_on, g_line_a, g_line_b;
static String g_ans_a, g_ans_b, g_gps;
static float g_bat_idle, g_bat_min_a, g_bat_min_b;
static uint32_t g_rx_bytes_a, g_rx_bytes_b;

/* ------------------------------------------------------------------------ */

static float vdda_mv()
{
    uint32_t raw = analogRead(AVREF);
    if (raw == 0) return 3300.0f;
    return (float)__LL_ADC_CALC_VREFANALOG_VOLTAGE(raw, LL_ADC_RESOLUTION_12B);
}

static float bat_v()
{
    /* same formula as stepwhere.ino: PB10 through a 1:2 divider */
    return analogRead(BAT_PIN) / 4095.0f * vdda_mv() / 1000.0f * 2.0f;
}

static void set_ldo(bool on)
{
    g_ldo = on;
    digitalWrite(LDO_EN, on ? HIGH : LOW);
    SerialDBG.print("  PA10 (modem power) = ");
    SerialDBG.println(on ? "HIGH (on)" : "LOW (off)");
}

static void set_key(bool high)
{
    g_key = high;
    digitalWrite(GSM_PWRKEY, high ? HIGH : LOW);
}

static void gsm_begin(uint32_t baud, bool swap)
{
    SerialGSM.end();
    SerialGSM.begin(baud);
    /* SWAP exchanges the TX and RX pin functions (PA2 <-> PA3).
     * It can only be changed while the USART is disabled. */
    USART2->CR1 &= ~USART_CR1_UE;
    if (swap) USART2->CR2 |= USART_CR2_SWAP;
    else      USART2->CR2 &= ~USART_CR2_SWAP;
    USART2->CR1 |= USART_CR1_UE;
    g_baud = baud;
    g_swap = swap;
    while (SerialGSM.available()) SerialGSM.read();
}

/* Is anybody driving the modem->MCU line (PA3)? Read it with the internal
 * pull-down and with the pull-up. Same value both times = driven. */
static String line_state()
{
    SerialGSM.end();
    pinMode(GSM_RX_PIN, INPUT_PULLDOWN);
    delay(5);
    int dn = digitalRead(GSM_RX_PIN);
    pinMode(GSM_RX_PIN, INPUT_PULLUP);
    delay(5);
    int up = digitalRead(GSM_RX_PIN);
    pinMode(GSM_RX_PIN, INPUT);
    gsm_begin(g_baud, g_swap);
    if (dn && up) return "driven HIGH (modem UART side is alive)";
    if (!dn && !up) return "driven LOW (held low: modem off with level shifter, or short)";
    return "floating (nobody drives it: modem/level shifter has no power)";
}

static void print_bytes(const uint8_t *b, size_t n)
{
    static const char hx[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '\n') SerialDBG.print("\\n");
        else if (c == '\r') SerialDBG.print("\\r");
        else if (c >= 32 && c < 127) SerialDBG.write(c);
        else { SerialDBG.print("\\x"); SerialDBG.write(hx[c >> 4]); SerialDBG.write(hx[c & 15]); }
    }
}

/* Listen on the modem UART; print what arrives with a time stamp.
 * Also sample the battery voltage. Returns number of bytes seen. */
static uint32_t listen_gsm(uint32_t ms, float *bat_min)
{
    uint8_t buf[96];
    size_t n = 0;
    uint32_t total = 0, t0 = millis(), last = millis();
    float vmin = 9.9f;
    while ((uint32_t)(millis() - t0) < ms) {
        while (SerialGSM.available()) {
            int c = SerialGSM.read();
            if (n < sizeof buf) buf[n++] = (uint8_t)c;
            total++;
            last = millis();
        }
        if (n && ((uint32_t)(millis() - last) > 100 || n == sizeof buf)) {
            SerialDBG.print("  [");
            SerialDBG.print((millis() - t0) / 1000.0f, 1);
            SerialDBG.print(" s] << ");
            print_bytes(buf, n);
            SerialDBG.println();
            n = 0;
        }
        float v = bat_v();
        if (v < vmin) vmin = v;
    }
    if (n) { SerialDBG.print("  << "); print_bytes(buf, n); SerialDBG.println(); }
    if (bat_min) *bat_min = vmin;
    SerialDBG.print("  listened ");
    SerialDBG.print(ms / 1000);
    SerialDBG.print(" s: ");
    SerialDBG.print(total);
    SerialDBG.print(" bytes, lowest battery ");
    SerialDBG.print(vmin, 2);
    SerialDBG.println(" V");
    return total;
}

/* Send AT and collect the answer for wait_ms. Returns true on "OK". */
static bool send_at(const char *cmd, uint32_t wait_ms, String *answer)
{
    while (SerialGSM.available()) SerialGSM.read();
    SerialGSM.print(cmd);
    SerialGSM.print("\r");
    uint8_t buf[96];
    size_t n = 0;
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < wait_ms) {
        while (SerialGSM.available() && n < sizeof buf) buf[n++] = (uint8_t)SerialGSM.read();
    }
    String s;
    for (size_t i = 0; i < n; i++) s += (char)buf[i];
    if (answer) *answer = s;
    SerialDBG.print("  >> ");
    SerialDBG.print(cmd);
    SerialDBG.print("  @");
    SerialDBG.print(g_baud);
    SerialDBG.print(g_swap ? " TX/RX swapped" : "");
    SerialDBG.print("  << ");
    if (n) print_bytes(buf, n); else SerialDBG.print("(nothing)");
    SerialDBG.println();
    return s.indexOf("OK") >= 0;
}

/* AT at 115200 normal, then a scan. Returns a short text result. */
static String find_modem()
{
    static const uint32_t bauds[] = {115200, 9600, 19200, 38400, 57600, 230400, 460800, 921600};
    for (int swap = 0; swap < 2; swap++) {
        for (unsigned i = 0; i < sizeof bauds / sizeof bauds[0]; i++) {
            gsm_begin(bauds[i], swap);
            String a;
            /* twice: modules with auto-baud need the first AT to lock on */
            bool ok = send_at("AT", 400, &a) || send_at("AT", 400, &a);
            if (ok) {
                String r = String("AT OK at ") + bauds[i] + (swap ? " with TX/RX SWAPPED" : "");
                send_at("ATI", 600, NULL);
                send_at("AT+CPIN?", 800, NULL);
                send_at("AT+CSQ", 600, NULL);
                send_at("AT+CGSN", 600, NULL);
                gsm_begin(115200, false);
                return r;
            }
        }
    }
    gsm_begin(115200, false);
    return "no answer at any baud rate, normal or swapped";
}

static void all_off()
{
    set_key(false);
    set_ldo(false);
}

static void run_test()
{
    SerialDBG.println("\n===== AUTOMATIC MODEM TEST (about 2 min, do not touch the board) =====");
    g_bat_idle = bat_v();
    SerialDBG.print("Battery (modem off): ");
    SerialDBG.print(g_bat_idle, 2);
    SerialDBG.println(" V");

    SerialDBG.println("\n[1] Modem power OFF for 5 s");
    all_off();
    delay(5000);
    g_line_off = line_state();
    SerialDBG.print("  PA3 line: ");
    SerialDBG.println(g_line_off);

    SerialDBG.println("\n[2] Modem power ON (PA10 HIGH), power key not pressed yet");
    set_ldo(true);
    delay(1000);
    g_line_on = line_state();
    SerialDBG.print("  PA3 line: ");
    SerialDBG.println(g_line_on);

    SerialDBG.println("\n[3] Method A (like firmware v1.2): PB13 idle HIGH, LOW for 1.2 s, back HIGH");
    set_key(true);
    delay(100);
    set_key(false);
    delay(1200);
    set_key(true);
    SerialDBG.println("  pulse done, listening for boot messages ...");
    g_rx_bytes_a = listen_gsm(15000, &g_bat_min_a);
    g_line_a = line_state();
    SerialDBG.print("  PA3 line: ");
    SerialDBG.println(g_line_a);
    g_ans_a = find_modem();
    SerialDBG.print("  RESULT A: ");
    SerialDBG.println(g_ans_a);

    SerialDBG.println("\n[4] Modem power OFF for 5 s");
    all_off();
    delay(5000);

    SerialDBG.println("\n[5] Method B (opposite): PB13 idle LOW, HIGH for 1.2 s, back LOW");
    set_ldo(true);
    delay(1000);
    set_key(true);
    delay(1200);
    set_key(false);
    SerialDBG.println("  pulse done, listening for boot messages ...");
    g_rx_bytes_b = listen_gsm(15000, &g_bat_min_b);
    g_line_b = line_state();
    SerialDBG.print("  PA3 line: ");
    SerialDBG.println(g_line_b);
    g_ans_b = find_modem();
    SerialDBG.print("  RESULT B: ");
    SerialDBG.println(g_ans_b);

    SerialDBG.println("\n[6] GPS: listening 5 s on USART3 (9600)");
    SerialGPS.begin(9600);
    uint8_t buf[200];
    size_t n = 0;
    uint32_t total = 0, t0 = millis();
    while ((uint32_t)(millis() - t0) < 5000) {
        while (SerialGPS.available()) {
            int c = SerialGPS.read();
            total++;
            if (n < sizeof buf) buf[n++] = (uint8_t)c;
        }
    }
    SerialDBG.print("  first bytes: ");
    if (n) print_bytes(buf, n); else SerialDBG.print("(nothing)");
    SerialDBG.println();
    g_gps = String(total) + " bytes in 5 s" + (total ? "" : " (GPS silent)");

    SerialDBG.println("\n===== SUMMARY (copy everything from here) =====");
    SerialDBG.println(VERSION);
    SerialDBG.print("Battery idle / lowest A / lowest B : ");
    SerialDBG.print(g_bat_idle, 2); SerialDBG.print(" / ");
    SerialDBG.print(g_bat_min_a, 2); SerialDBG.print(" / ");
    SerialDBG.print(g_bat_min_b, 2); SerialDBG.println(" V");
    SerialDBG.print("PA3 power off      : "); SerialDBG.println(g_line_off);
    SerialDBG.print("PA3 power on       : "); SerialDBG.println(g_line_on);
    SerialDBG.print("PA3 after method A : "); SerialDBG.println(g_line_a);
    SerialDBG.print("PA3 after method B : "); SerialDBG.println(g_line_b);
    SerialDBG.print("Boot bytes A / B   : "); SerialDBG.print(g_rx_bytes_a);
    SerialDBG.print(" / "); SerialDBG.println(g_rx_bytes_b);
    SerialDBG.print("Method A           : "); SerialDBG.println(g_ans_a);
    SerialDBG.print("Method B           : "); SerialDBG.println(g_ans_b);
    SerialDBG.print("GPS                : "); SerialDBG.println(g_gps);
    SerialDBG.println("Modem left: power ON, PB13 LOW. Keys: h for help.");
    SerialDBG.println("===== END =====");
}

static void pass_through()
{
    SerialDBG.print("AT pass-through @");
    SerialDBG.print(g_baud);
    SerialDBG.println(". Type AT commands, Enter sends. Type ~~~ to leave.");
    int tildes = 0;
    for (;;) {
        while (SerialGSM.available()) SerialDBG.write(SerialGSM.read());
        if (SerialDBG.available()) {
            int c = SerialDBG.read();
            if (c == '~') { if (++tildes == 3) break; continue; }
            while (tildes) { SerialGSM.write('~'); tildes--; }
            if (c == '\n') continue;
            SerialDBG.write(c == '\r' ? '\n' : c);
            SerialGSM.write(c);
        }
    }
    SerialDBG.println("\nleft pass-through");
}

static void pin_info()
{
    SerialDBG.print("PA10 = "); SerialDBG.print(g_ldo ? "HIGH" : "LOW");
    SerialDBG.print(", PB13 = "); SerialDBG.print(g_key ? "HIGH" : "LOW");
    SerialDBG.print(", battery = "); SerialDBG.print(bat_v(), 2);
    SerialDBG.print(" V, VDDA = "); SerialDBG.print(vdda_mv() / 1000.0f, 2);
    SerialDBG.println(" V");
    SerialDBG.print("PA3 line: "); SerialDBG.println(line_state());
}

static void help()
{
    SerialDBG.println(VERSION);
    SerialDBG.println(" t  full automatic test (about 2 min)");
    SerialDBG.println(" l  toggle PA10 (modem/GPS power)   k  toggle PB13 (power key)");
    SerialDBG.println(" a  send AT at 115200               w  listen to modem 15 s");
    SerialDBG.println(" g  listen to GPS 5 s               m  AT pass-through (~~~ leaves)");
    SerialDBG.println(" i  pin states                      r  reset   h  help");
}

void setup()
{
    pinMode(LDO_EN, OUTPUT);
    digitalWrite(LDO_EN, LOW);
    pinMode(GSM_PWRKEY, OUTPUT);
    digitalWrite(GSM_PWRKEY, LOW);
    analogReadResolution(12);
    pinMode(BAT_PIN, INPUT_ANALOG);

    SerialDBG.begin(115200);
    delay(200);
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;
    SerialDBG.println();
    SerialDBG.println(VERSION);
    SerialDBG.print("reset cause RCC_CSR = 0x");
    SerialDBG.print(csr >> 24, HEX);
    SerialDBG.print((csr & RCC_CSR_PWRRSTF) ? " POWER/brown-out" : "");
    SerialDBG.print((csr & RCC_CSR_PINRSTF) ? " PIN" : "");
    SerialDBG.print((csr & RCC_CSR_SFTRSTF) ? " SOFTWARE" : "");
    SerialDBG.print((csr & RCC_CSR_IWDGRSTF) ? " WATCHDOG" : "");
    SerialDBG.println();
    gsm_begin(115200, false);
    help();
    SerialDBG.println("Press t to start the test.");
}

void loop()
{
    if (!SerialDBG.available()) return;
    switch (SerialDBG.read()) {
    case 't': run_test(); break;
    case 'l': set_ldo(!g_ldo); break;
    case 'k': set_key(!g_key); SerialDBG.print("  PB13 = "); SerialDBG.println(g_key ? "HIGH" : "LOW"); break;
    case 'a': gsm_begin(115200, false); send_at("AT", 500, NULL); break;
    case 'w': listen_gsm(15000, NULL); break;
    case 'g': {
        SerialGPS.begin(9600);
        uint32_t t0 = millis(), total = 0;
        while ((uint32_t)(millis() - t0) < 5000)
            while (SerialGPS.available()) { SerialDBG.write(SerialGPS.read()); total++; }
        SerialDBG.print("\n  GPS bytes in 5 s: ");
        SerialDBG.println(total);
        break;
    }
    case 'm': pass_through(); break;
    case 'i': pin_info(); break;
    case 'r': NVIC_SystemReset(); break;
    case 'h': help(); break;
    default: break;
    }
}
