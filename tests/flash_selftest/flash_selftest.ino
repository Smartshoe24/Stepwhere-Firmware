/*
 * flash_selftest.ino - Checks the W25Q128 external flash on the GSM PCB.
 *
 * Flash this INSTEAD of the normal firmware, open the debug UART
 * (USART1, PB6 TX / PB7 RX, 115200 baud) and send the output to the team.
 *
 * It only erases and writes the last 64 KB block of the flash
 * (0xFF0000-0xFFFFFF), which is outside every OTA region, so it is safe to
 * run on any board.
 *
 * Requires the StepwhereOTA library from libraries/StepwhereOTA.
 */
#include <HardwareSerial.h>
#include <W25QArduino.h>
#include <ota_layout.h>
#include <w25q.h>

// ---------------------------------------------------------------------------
// Pin mapping from the GSM PCB schematic (IC8 = W25Q128, IC4 = STM32G070CB)
// ---------------------------------------------------------------------------
#define FLASH_MOSI PA7   // MCU pin 18 -> W25Q pin 5 (DI),    SPI1_MOSI
#define FLASH_MISO PA6   // MCU pin 17 <- W25Q pin 2 (DO),    SPI1_MISO
#define FLASH_SCK  PA5   // MCU pin 16 -> W25Q pin 6 (CLK),   SPI1_SCK
#define FLASH_CS   PC6   // MCU pin 30 -> W25Q pin 1 (/CS)
#define FLASH_WP   PC7   // MCU pin 31 -> W25Q pin 3 (/WP),   driven HIGH
#define FLASH_HOLD PB12  // MCU pin 24 -> W25Q pin 7 (/HOLD), driven HIGH

#define LDO_EN     PA10  // GSM/GPS LDO

HardwareSerial SerialDBG(PB7, PB6);  // USART1 debug console (RX, TX)
static W25QArduino flash(FLASH_MOSI, FLASH_MISO, FLASH_SCK, FLASH_CS, FLASH_WP, FLASH_HOLD);

static int g_failures = 0;

static void report(const char *step, w25q_status_t st)
{
    SerialDBG.print(st == W25Q_OK ? "[PASS] " : "[FAIL] ");
    SerialDBG.print(step);
    if (st != W25Q_OK) {
        SerialDBG.print(" -> ");
        SerialDBG.print(w25q_status_str(st));
        g_failures++;
    }
    SerialDBG.println();
}

static void print_hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 0x10) SerialDBG.print('0');
        SerialDBG.print(p[i], HEX);
    }
}

static bool all_equal(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != v) return false;
    }
    return true;
}

static void run_selftest()
{
    static uint8_t wbuf[W25Q_SECTOR_SIZE];
    static uint8_t rbuf[W25Q_SECTOR_SIZE];
    w25q_t *dev = flash.dev();
    const uint32_t addr = EXT_SELFTEST_ADDR;
    w25q_status_t st;
    uint32_t t0;

    SerialDBG.println();
    SerialDBG.println("=== Stepwhere W25Q128 flash self-test ===");

    // 1. Init + JEDEC ID. First with the GSM/GPS LDO off. If the flash does
    //    not answer, it may be powered from that LDO: switch it on and retry.
    //    The result tells us which 3.3 V rail the flash is on.
    uint8_t manuf;
    uint16_t dev_id;
    digitalWrite(LDO_EN, LOW);
    delay(20);
    st = flash.begin();
    w25q_read_jedec_id(dev, &manuf, &dev_id);  // re-read so it prints even if init failed
    SerialDBG.print("JEDEC ID (LDO_EN off): ");
    SerialDBG.print(manuf, HEX);
    SerialDBG.print(' ');
    SerialDBG.println(dev_id, HEX);

    if (st == W25Q_OK) {
        SerialDBG.println("Flash power: always-on 3.3 V (works with LDO_EN off)");
    } else {
        digitalWrite(LDO_EN, HIGH);
        delay(100);                            // same settle time as ldo_enable()
        st = flash.begin();
        w25q_read_jedec_id(dev, &manuf, &dev_id);
        SerialDBG.print("JEDEC ID (LDO_EN on):  ");
        SerialDBG.print(manuf, HEX);
        SerialDBG.print(' ');
        SerialDBG.println(dev_id, HEX);
        if (st == W25Q_OK) {
            SerialDBG.println("Flash power: switched by LDO_EN (PA10)");
        }
    }
    SerialDBG.println("  expected EF 4018 (W25Q128FV/JV-IQ) or EF 7018 (JV-IM)");
    SerialDBG.println("  FF FFFF = no answer (check MISO/CS wiring, /HOLD, power)");
    SerialDBG.println("  0 0     = MISO stuck low");
    report("init + JEDEC ID", st);
    if (st != W25Q_OK) {
        digitalWrite(LDO_EN, LOW);
        return;  // nothing else can work
    }

    // 2. Unique ID (different on every chip)
    uint8_t uid[8];
    w25q_read_unique_id(dev, uid);
    SerialDBG.print("Unique ID: ");
    print_hex(uid, sizeof uid);
    SerialDBG.println();

    // 3. Erase the scratch sector and check it reads back as 0xFF
    t0 = millis();
    st = w25q_erase_sector(dev, addr);
    SerialDBG.print("Sector erase took ");
    SerialDBG.print(millis() - t0);
    SerialDBG.println(" ms (datasheet: 45 typ, 400 max)");
    report("erase 4 KB sector", st);
    if (st == W25Q_OK) {
        w25q_read(dev, addr, rbuf, sizeof rbuf);
        report("sector reads 0xFF after erase",
               all_equal(rbuf, sizeof rbuf, 0xFF) ? W25Q_OK : W25Q_ERR_VERIFY);
    }

    // 4. Write a pattern across all 16 pages, starting at an odd offset so
    //    page-boundary splitting is exercised, then verify.
    for (size_t i = 0; i < sizeof wbuf; i++) {
        wbuf[i] = (uint8_t)((i * 7u) ^ (i >> 8) ^ 0xA5u);
    }
    const size_t off = 3;
    t0 = millis();
    st = w25q_write_verify(dev, addr + off, wbuf, sizeof wbuf - off);
    SerialDBG.print("Write+verify of ");
    SerialDBG.print(sizeof wbuf - off);
    SerialDBG.print(" bytes took ");
    SerialDBG.print(millis() - t0);
    SerialDBG.println(" ms");
    report("write + read-back pattern", st);

    // 5. Bytes before the write offset must still be erased
    w25q_read(dev, addr, rbuf, off);
    report("untouched bytes still 0xFF", all_equal(rbuf, off, 0xFF) ? W25Q_OK : W25Q_ERR_VERIFY);

    // 6. Power-down / wake cycle, data must survive
    report("power-down", w25q_power_down(dev));
    delay(10);
    w25q_read(dev, addr + off, rbuf, 64);  // wakes the chip automatically
    report("data intact after power-down",
           memcmp(rbuf, wbuf, 64) == 0 ? W25Q_OK : W25Q_ERR_VERIFY);

    // 7. 64 KB block erase timing on the last block (also outside OTA areas)
    t0 = millis();
    st = w25q_erase_block64(dev, EXT_FLASH_SIZE - EXT_BLOCK_SIZE);
    SerialDBG.print("64 KB block erase took ");
    SerialDBG.print(millis() - t0);
    SerialDBG.println(" ms (datasheet: 150 typ, 2000 max)");
    report("erase 64 KB block", st);

    // Leave the chip in low-power mode and the LDO off
    w25q_power_down(dev);
    digitalWrite(LDO_EN, LOW);

    SerialDBG.println();
    SerialDBG.println(g_failures == 0 ? "RESULT: ALL TESTS PASSED" : "RESULT: FAILED");
}

void setup()
{
    pinMode(LDO_EN, OUTPUT);
    digitalWrite(LDO_EN, LOW);

    SerialDBG.begin(115200);
    delay(500);
    run_selftest();
}

void loop()
{
    // Re-run on any key press from the debug console.
    if (SerialDBG.available()) {
        while (SerialDBG.available()) SerialDBG.read();
        g_failures = 0;
        run_selftest();
    }
}
