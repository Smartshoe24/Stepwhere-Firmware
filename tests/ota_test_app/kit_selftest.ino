/*
 * kit_selftest.ino - W25Q128 self-test built into the OTA test app ('s').
 * Uses only the last 64 KB of the chip (0xFF0000..), outside every OTA area.
 */

static uint8_t g_st_w[W25Q_SECTOR_SIZE];
static uint8_t g_st_r[W25Q_SECTOR_SIZE];
static int g_st_fail;

static void st_report(const char *step, w25q_status_t st)
{
    SerialDBG.print(st == W25Q_OK ? "[PASS] " : "[FAIL] ");
    SerialDBG.print(step);
    if (st != W25Q_OK) {
        SerialDBG.print(" -> ");
        SerialDBG.print(w25q_status_str(st));
        g_st_fail++;
    }
    SerialDBG.println();
}

static bool st_all(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++) if (p[i] != v) return false;
    return true;
}

void kit_selftest()
{
    w25q_t *dev = flash.dev();
    const uint32_t addr = EXT_SELFTEST_ADDR;
    uint8_t manuf = 0;
    uint16_t id = 0;
    uint32_t t0;
    w25q_status_t st;

    g_st_fail = 0;
    SerialDBG.println("=== Stepwhere W25Q128 flash self-test ===");
    w25q_read_jedec_id(dev, &manuf, &id);
    SerialDBG.print("JEDEC ID: ");
    SerialDBG.print(manuf, HEX);
    SerialDBG.print(' ');
    SerialDBG.println(id, HEX);
    st_report("init + JEDEC ID", g_flash_ok ? W25Q_OK : W25Q_ERR_ID);
    if (!g_flash_ok) {
        SerialDBG.println("RESULT: FAILED");
        return;
    }

    wdg_kick();
    t0 = millis();
    st = w25q_erase_sector(dev, addr);
    SerialDBG.print("Sector erase took ");
    SerialDBG.print(millis() - t0);
    SerialDBG.println(" ms");
    st_report("erase 4 KB sector", st);
    w25q_read(dev, addr, g_st_r, sizeof g_st_r);
    st_report("sector reads 0xFF after erase", st_all(g_st_r, sizeof g_st_r, 0xFF) ? W25Q_OK : W25Q_ERR_VERIFY);

    for (size_t i = 0; i < sizeof g_st_w; i++) g_st_w[i] = (uint8_t)((i * 7u) ^ (i >> 8) ^ 0xA5u);
    const size_t off = 3;
    wdg_kick();
    t0 = millis();
    st = w25q_write_verify(dev, addr + off, g_st_w, sizeof g_st_w - off);
    SerialDBG.print("Write+verify took ");
    SerialDBG.print(millis() - t0);
    SerialDBG.println(" ms");
    st_report("write + read-back pattern", st);
    w25q_read(dev, addr, g_st_r, off);
    st_report("untouched bytes still 0xFF", st_all(g_st_r, off, 0xFF) ? W25Q_OK : W25Q_ERR_VERIFY);

    st_report("power-down", w25q_power_down(dev));
    delay(10);
    w25q_read(dev, addr + off, g_st_r, 64);
    st_report("data intact after power-down", memcmp(g_st_r, g_st_w, 64) == 0 ? W25Q_OK : W25Q_ERR_VERIFY);

    wdg_kick();
    t0 = millis();
    st = w25q_erase_block64(dev, EXT_FLASH_SIZE - EXT_BLOCK_SIZE);
    SerialDBG.print("64 KB block erase took ");
    SerialDBG.print(millis() - t0);
    SerialDBG.println(" ms");
    st_report("erase 64 KB block", st);
    wdg_kick();

    SerialDBG.println(g_st_fail == 0 ? "RESULT: ALL TESTS PASSED" : "RESULT: FAILED");
}
