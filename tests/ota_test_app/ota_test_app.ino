/*
 * ota_test_app.ino - Stand-in application for testing the OTA bootloader.
 *
 * Built at 0x08004000 (behind the bootloader). Talks on the debug UART
 * (USART1, PB6 TX / PB7 RX, 115200 baud). Commands:
 *
 *   i   show OTA state (boot metadata and image slots)
 *   u   receive a .ota file by XMODEM into the download slot, check it,
 *       and mark it PENDING (installed by the bootloader on the next reset)
 *   r   reset
 *   e   erase the boot metadata (back to "first boot")
 *   h   help
 *
 * Build options (passed with -D, see tests/ota_test_app/build.sh):
 *   TEST_APP_VERSION_MAJOR / _MINOR / _PATCH   version printed at start
 *   TEST_APP_NO_CONFIRM=1   never confirm, and reset after 10 s. Simulates a
 *                           broken update: the bootloader must roll back.
 */
#include <HardwareSerial.h>
#include <W25QArduino.h>
#include <ota_image.h>
#include <ota_layout.h>
#include <ota_meta.h>
#include <w25q.h>

#ifndef TEST_APP_VERSION_MAJOR
#define TEST_APP_VERSION_MAJOR 1
#endif
#ifndef TEST_APP_VERSION_MINOR
#define TEST_APP_VERSION_MINOR 0
#endif
#ifndef TEST_APP_VERSION_PATCH
#define TEST_APP_VERSION_PATCH 0
#endif
#ifndef TEST_APP_NO_CONFIRM
#define TEST_APP_NO_CONFIRM 0
#endif

#define APP_VERSION OTA_VERSION(TEST_APP_VERSION_MAJOR, TEST_APP_VERSION_MINOR, TEST_APP_VERSION_PATCH)

/* Pins: same as tests/flash_selftest and bootloader/src/board.h */
HardwareSerial SerialDBG(PB7, PB6);
static W25QArduino flash(PA7, PA6, PA5, PC6, PC7, PB12);
static bool g_flash_ok;

static uint8_t g_buf[1024];   /* XMODEM block and scratch for slot checks */

/* ------------------------------------------------------------------------ */

static void print_version(uint32_t v)
{
    SerialDBG.print((v >> 16) & 0xFF);
    SerialDBG.print('.');
    SerialDBG.print((v >> 8) & 0xFF);
    SerialDBG.print('.');
    SerialDBG.print(v & 0xFF);
}

static const char *result_str(uint32_t r)
{
    switch (r) {
    case OTA_RES_NONE:           return "none";
    case OTA_RES_INSTALLED:      return "installed (waiting for confirm)";
    case OTA_RES_CONFIRMED:      return "confirmed";
    case OTA_RES_BAD_IMAGE:      return "rejected bad image";
    case OTA_RES_INSTALL_FAILED: return "install failed";
    case OTA_RES_ROLLED_BACK:    return "rolled back";
    case OTA_RES_NO_BACKUP:      return "no backup to restore";
    default:                     return "?";
    }
}

static void show_slot(const char *name, uint32_t addr)
{
    ota_image_header_t h;
    ota_img_status_t st = ota_image_check_slot(flash.dev(), addr, &h, g_buf, sizeof g_buf);
    SerialDBG.print("  ");
    SerialDBG.print(name);
    SerialDBG.print(" slot: ");
    if (st == OTA_IMG_OK) {
        SerialDBG.print('v');
        print_version(h.fw_version);
        SerialDBG.print(", ");
        SerialDBG.print(h.image_size);
        SerialDBG.println(" bytes, valid");
    } else {
        SerialDBG.println(ota_img_status_str(st));
    }
}

static void cmd_info()
{
    ota_boot_meta_t m;
    if (!g_flash_ok) {
        SerialDBG.println("external flash not available");
        return;
    }
    bool have = ota_meta_load(flash.dev(), &m);
    SerialDBG.println("OTA state:");
    if (!have) {
        SerialDBG.println("  no metadata (first boot, app was flashed by cable)");
    } else {
        SerialDBG.print("  state        ");
        SerialDBG.println(ota_state_str(m.state));
        SerialDBG.print("  installed    v");
        print_version(m.installed_version);
        SerialDBG.println();
        SerialDBG.print("  trial boots  ");
        SerialDBG.println(m.trial_boots);
        SerialDBG.print("  last result  ");
        SerialDBG.println(result_str(m.last_result));
        SerialDBG.print("  seq          ");
        SerialDBG.println(m.seq);
    }
    show_slot("download", EXT_SLOT_DOWNLOAD);
    show_slot("backup  ", EXT_SLOT_BACKUP);
}

/* ------------------------------------------------------------------------ */
/* XMODEM-CRC / XMODEM-1K receiver. Works with Tera Term:                    */
/* File > Transfer > XMODEM > Send, tick "1K".                               */

#define X_SOH 0x01
#define X_STX 0x02
#define X_EOT 0x04
#define X_ACK 0x06
#define X_NAK 0x15
#define X_CAN 0x18

static int read_byte(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < timeout_ms) {
        if (SerialDBG.available()) return SerialDBG.read();
    }
    return -1;
}

static uint16_t crc16_xmodem(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void flush_rx(uint32_t quiet_ms)
{
    while (read_byte(quiet_ms) >= 0) { }
}

/* Returns bytes received, or -1 on error. Writes into the download slot. */
static long xmodem_receive()
{
    uint32_t offset = 0;
    uint8_t expected = 1;
    int start = -1;

    /* Ask for CRC mode for up to 60 s. */
    for (int tries = 0; tries < 60 && start < 0; tries++) {
        SerialDBG.write('C');
        start = read_byte(1000);
        if (start != X_SOH && start != X_STX && start != X_EOT) start = -1;
    }
    if (start < 0) return -1;

    for (;;) {
        int c = start >= 0 ? start : read_byte(10000);
        start = -1;
        if (c == X_EOT) {
            SerialDBG.write(X_ACK);
            return (long)offset;
        }
        if (c == X_CAN) return -1;
        if (c != X_SOH && c != X_STX) {
            if (c < 0) return -1;
            continue;                   /* noise between blocks */
        }
        size_t len = c == X_STX ? 1024 : 128;
        int blk = read_byte(1000), nblk = read_byte(1000);
        bool ok = blk >= 0 && nblk >= 0 && (uint8_t)(blk + nblk) == 0xFF;
        for (size_t i = 0; i < len && ok; i++) {
            int b = read_byte(1000);
            if (b < 0) ok = false; else g_buf[i] = (uint8_t)b;
        }
        int ch = ok ? read_byte(1000) : -1, cl = ok ? read_byte(1000) : -1;
        ok = ok && ch >= 0 && cl >= 0 && crc16_xmodem(g_buf, len) == (uint16_t)((ch << 8) | cl);
        if (!ok) {
            flush_rx(50);
            SerialDBG.write(X_NAK);
            continue;
        }
        if ((uint8_t)blk == (uint8_t)(expected - 1)) {   /* repeat of last block */
            SerialDBG.write(X_ACK);
            continue;
        }
        if ((uint8_t)blk != expected || offset + len > EXT_SLOT_SIZE ||
            w25q_write_verify(flash.dev(), EXT_SLOT_DOWNLOAD + offset, g_buf, len) != W25Q_OK) {
            for (int i = 0; i < 3; i++) SerialDBG.write(X_CAN);
            return -1;
        }
        offset += len;
        expected++;
        SerialDBG.write(X_ACK);
    }
}

static void cmd_upload()
{
    if (!g_flash_ok) {
        SerialDBG.println("external flash not available");
        return;
    }
    SerialDBG.println("Erasing download slot...");
    if (w25q_erase_range(flash.dev(), EXT_SLOT_DOWNLOAD, EXT_SLOT_SIZE) != W25Q_OK) {
        SerialDBG.println("erase failed");
        return;
    }
    SerialDBG.println("Ready. Send the .ota file now: Tera Term > File > Transfer > XMODEM > Send (tick 1K).");
    long n = xmodem_receive();
    delay(500);
    flush_rx(100);
    SerialDBG.println();
    if (n < 0) {
        SerialDBG.println("Transfer failed or cancelled.");
        return;
    }
    SerialDBG.print("Received ");
    SerialDBG.print(n);
    SerialDBG.println(" bytes. Checking image...");

    ota_image_header_t h;
    ota_img_status_t st = ota_image_check_slot(flash.dev(), EXT_SLOT_DOWNLOAD, &h, g_buf, sizeof g_buf);
    if (st != OTA_IMG_OK) {
        SerialDBG.print("Image REJECTED: ");
        SerialDBG.println(ota_img_status_str(st));
        return;
    }
    SerialDBG.print("Image OK: v");
    print_version(h.fw_version);
    SerialDBG.print(", ");
    SerialDBG.print(h.image_size);
    SerialDBG.println(" bytes");
    if (ota_meta_set_pending(flash.dev(), h.fw_version) != W25Q_OK) {
        SerialDBG.println("could not write metadata");
        return;
    }
    SerialDBG.println("Marked PENDING. Press 'r' (or reset the board) to install.");
}

static void cmd_erase_meta()
{
    if (!g_flash_ok) return;
    w25q_erase_sector(flash.dev(), EXT_META_A_ADDR);
    w25q_erase_sector(flash.dev(), EXT_META_B_ADDR);
    SerialDBG.println("Metadata erased.");
}

static void help()
{
    SerialDBG.println("Commands: i = OTA info, u = upload .ota by XMODEM, r = reset, e = erase metadata, h = help");
}

/* ------------------------------------------------------------------------ */

void setup()
{
    SerialDBG.begin(115200);
    delay(100);
    SerialDBG.println();
    SerialDBG.println("==============================================");
    SerialDBG.print("  OTA TEST APP v");
    print_version(APP_VERSION);
#if TEST_APP_NO_CONFIRM
    SerialDBG.print("  (NO-CONFIRM build)");
#endif
    SerialDBG.println();
    SerialDBG.print("  running from 0x");
    SerialDBG.print(SCB->VTOR, HEX);
    SerialDBG.println(" (VTOR)");
    SerialDBG.println("==============================================");

    g_flash_ok = flash.begin() == W25Q_OK;
    if (!g_flash_ok) {
        SerialDBG.println("external flash NOT found");
    }

#if TEST_APP_NO_CONFIRM
    SerialDBG.println("Not confirming this firmware. Resetting in 10 s to simulate a crash.");
#else
    if (g_flash_ok) {
        ota_boot_meta_t m;
        if (ota_meta_load(flash.dev(), &m) && m.state == OTA_STATE_TRIAL) {
            /* A real app would run its self-test (GSM, GPS, sensor) first. */
            ota_meta_confirm(flash.dev());
            SerialDBG.println("New firmware confirmed.");
        }
    }
#endif
    cmd_info();
    help();
}

void loop()
{
    static uint32_t last_beat;
    if ((uint32_t)(millis() - last_beat) >= 5000) {
        last_beat = millis();
        SerialDBG.print("[v");
        print_version(APP_VERSION);
        SerialDBG.print("] alive, uptime ");
        SerialDBG.print(millis() / 1000);
        SerialDBG.println(" s");
    }
#if TEST_APP_NO_CONFIRM
    if (millis() > 10000) {
        SerialDBG.println("Simulated crash: resetting.");
        SerialDBG.flush();
        NVIC_SystemReset();
    }
#endif
    if (!SerialDBG.available()) return;
    switch (SerialDBG.read()) {
    case 'i': case 'I': cmd_info(); break;
    case 'u': case 'U': cmd_upload(); break;
    case 'r': case 'R':
        SerialDBG.println("Resetting...");
        SerialDBG.flush();
        NVIC_SystemReset();
        break;
    case 'e': case 'E': cmd_erase_meta(); break;
    case 'h': case 'H': case '?': help(); break;
    default: break;
    }
}
