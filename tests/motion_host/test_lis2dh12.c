/*
 * test_lis2dh12.c - Host unit tests for the lis2dh12 driver, run against the
 * simulated chip in fake_lis2dh12.c.
 *
 * Build and run:  tests/motion_host/run_tests.sh
 */
#include <stdio.h>
#include <stdlib.h>

#include "lis2dh12.h"
#include "fake_lis2dh12.h"

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static lis2dh12_config_t step_cfg(void)
{
    lis2dh12_config_t c = { 0 };
    c.odr = LIS2DH12_ODR_25HZ;
    c.mode = LIS2DH12_MODE_NORMAL;
    c.fs = LIS2DH12_FS_8G;
    c.fifo_watermark = 24;
    c.int1_watermark = true;
    c.wake_threshold_mg = 250;
    c.wake_duration = 1;
    c.int_active_low = true;
    return c;
}

static void test_init(void)
{
    lis2dh12_t d;
    fake_lis_reset();
    FL.reg[LIS2DH12_CTRL_REG3] = 0x40;          /* left over from before an MCU reset */
    FL.reg[LIS2DH12_CTRL_REG1] = 0x57;
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, LIS2DH12_ADDR_SA0_HIGH) == LIS2DH12_OK);
    CHECK(FL.n_boot == 1);
    CHECK(FL.reg[LIS2DH12_CTRL_REG1] == 0x00);  /* powered down */
    CHECK(FL.reg[LIS2DH12_CTRL_REG3] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG6] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG0] == 0x10);  /* reserved bits kept */
    CHECK(!d.configured);

    fake_lis_reset();
    FL.who_am_i = 0x43;                         /* LIS2DS12 answers 0x43 */
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, LIS2DH12_ADDR_SA0_HIGH) == LIS2DH12_ERR_ID);

    fake_lis_reset();
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, LIS2DH12_ADDR_SA0_LOW) == LIS2DH12_ERR_BUS);  /* wrong address */

    fake_lis_reset();
    FL.nack = true;
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, LIS2DH12_ADDR_SA0_HIGH) == LIS2DH12_ERR_BUS);

    CHECK(lis2dh12_init(&d, NULL, 0x19) == LIS2DH12_ERR_PARAM);
    CHECK(lis2dh12_init(NULL, &FAKE_LIS_BUS, 0x19) == LIS2DH12_ERR_PARAM);
}

static void test_configure_registers(void)
{
    lis2dh12_t d;
    fake_lis_reset();
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, 0x19) == LIS2DH12_OK);
    lis2dh12_config_t c = step_cfg();
    int start = FL.n_log;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);

    CHECK(FL.reg[LIS2DH12_CTRL_REG1] == 0x37);  /* 25 Hz, normal, XYZ on      */
    CHECK(FL.reg[LIS2DH12_CTRL_REG4] == 0xA0);  /* BDU, +-8 g, HR off          */
    CHECK(FL.reg[LIS2DH12_CTRL_REG2] == 0x02);  /* HP filter on INT2 only      */
    CHECK(FL.reg[LIS2DH12_CTRL_REG5] == 0x40);  /* FIFO enabled, no latching   */
    CHECK(FL.reg[LIS2DH12_FIFO_CTRL_REG] == (0x80 | 24));
    CHECK(FL.reg[LIS2DH12_INT2_CFG] == 0x2A);   /* X/Y/Z high, OR              */
    CHECK(FL.reg[LIS2DH12_INT2_THS] == 5);      /* 250 mg / 62 mg -> 5 (310 mg)*/
    CHECK(FL.reg[LIS2DH12_INT2_DURATION] == 1);
    CHECK(FL.reg[LIS2DH12_CTRL_REG3] == 0x04);  /* watermark -> INT1           */
    CHECK(FL.reg[LIS2DH12_CTRL_REG6] == 0x22);  /* IA2 -> INT2, active low     */

    /* Interrupts are routed only after the sensor is set up and started. */
    int i_ctrl1 = -1;
    for (int i = start; i < FL.n_log; i++)
        if (FL.log[i].reg == LIS2DH12_CTRL_REG1 && FL.log[i].val == 0x37) i_ctrl1 = i;
    int i_route3 = -1, i_route6 = -1;
    for (int i = start; i < FL.n_log; i++) {
        if (FL.log[i].reg == LIS2DH12_CTRL_REG3 && FL.log[i].val == 0x04) i_route3 = i;
        if (FL.log[i].reg == LIS2DH12_CTRL_REG6 && FL.log[i].val == 0x22) i_route6 = i;
    }
    CHECK(i_ctrl1 > 0 && i_route3 > i_ctrl1 && i_route6 > i_ctrl1);
    /* ... and the first thing done is switching them off. */
    CHECK(FL.log[start].reg == LIS2DH12_CTRL_REG3 && FL.log[start].val == 0x00);

    /* Other modes */
    c.mode = LIS2DH12_MODE_HIGH_RES; c.odr = LIS2DH12_ODR_50HZ; c.fs = LIS2DH12_FS_2G;
    c.wake_threshold_mg = 0; c.int1_watermark = false; c.fifo_watermark = 0; c.int_active_low = false;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    CHECK(FL.reg[LIS2DH12_CTRL_REG1] == 0x47);
    CHECK(FL.reg[LIS2DH12_CTRL_REG4] == 0x88);
    CHECK(FL.reg[LIS2DH12_CTRL_REG5] == 0x00);
    CHECK(FL.reg[LIS2DH12_INT2_CFG] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG2] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG3] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG6] == 0x00);

    c.mode = LIS2DH12_MODE_LOW_POWER; c.odr = LIS2DH12_ODR_10HZ;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    CHECK(FL.reg[LIS2DH12_CTRL_REG1] == 0x2F);

    /* Threshold clamps to the 7-bit register. */
    c = step_cfg(); c.fs = LIS2DH12_FS_2G; c.wake_threshold_mg = 5000;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    CHECK(FL.reg[LIS2DH12_INT2_THS] == 127);
    c.wake_threshold_mg = 1;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    CHECK(FL.reg[LIS2DH12_INT2_THS] == 1);
}

static void test_configure_rejects(void)
{
    lis2dh12_t d;
    fake_lis_reset();
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, 0x19) == LIS2DH12_OK);
    lis2dh12_config_t c = step_cfg();
    c.fifo_watermark = 32;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_ERR_PARAM);
    c = step_cfg(); c.fifo_watermark = 0;         /* watermark interrupt without FIFO */
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_ERR_PARAM);
    c = step_cfg(); c.odr = LIS2DH12_ODR_OFF;     /* wake-up needs a running sensor */
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_ERR_PARAM);
    c = step_cfg(); c.odr = (lis2dh12_odr_t)9;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_ERR_PARAM);
    CHECK(lis2dh12_configure(&d, NULL) == LIS2DH12_ERR_PARAM);
    lis2dh12_sample_t s;
    CHECK(lis2dh12_read_sample(&d, &s) == LIS2DH12_ERR_PARAM);   /* not configured yet */
}

static void test_conversion(void)
{
    /* raw words are left-justified */
    CHECK(lis2dh12_raw_to_mg((int16_t)(250 << 6), LIS2DH12_MODE_NORMAL, LIS2DH12_FS_2G) == 1000);
    CHECK(lis2dh12_raw_to_mg((int16_t)(-250 * 64), LIS2DH12_MODE_NORMAL, LIS2DH12_FS_2G) == -1000);
    CHECK(lis2dh12_raw_to_mg((int16_t)(1000 << 4), LIS2DH12_MODE_HIGH_RES, LIS2DH12_FS_2G) == 1000);
    CHECK(lis2dh12_raw_to_mg((int16_t)(250 << 4), LIS2DH12_MODE_HIGH_RES, LIS2DH12_FS_8G) == 1000);
    CHECK(lis2dh12_raw_to_mg((int16_t)(62 << 6), LIS2DH12_MODE_NORMAL, LIS2DH12_FS_8G) == 992);
    CHECK(lis2dh12_raw_to_mg((int16_t)(-128 * 256), LIS2DH12_MODE_LOW_POWER, LIS2DH12_FS_16G) == -24576);
    CHECK(lis2dh12_raw_to_mg((int16_t)(127 << 8), LIS2DH12_MODE_LOW_POWER, LIS2DH12_FS_16G) == 24384);
    /* low bits that are not part of the 10-bit value are ignored */
    CHECK(lis2dh12_raw_to_mg((int16_t)((250 << 6) | 0x3F), LIS2DH12_MODE_NORMAL, LIS2DH12_FS_2G) == 1000);
    CHECK(lis2dh12_odr_hz(LIS2DH12_ODR_25HZ) == 25 && lis2dh12_odr_hz(LIS2DH12_ODR_400HZ) == 400);
}

static void test_single_sample(void)
{
    lis2dh12_t d;
    fake_lis_reset();
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, 0x19) == LIS2DH12_OK);
    lis2dh12_config_t c = step_cfg();
    c.fifo_watermark = 0; c.int1_watermark = false;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    fake_lis_push_mg(16, -1008, 3200);
    lis2dh12_sample_t s;
    CHECK(lis2dh12_read_sample(&d, &s) == LIS2DH12_OK);
    CHECK(s.x == 16 && s.y == -1008 && s.z == 3200);
    fake_lis_push_mg(9000, 0, 0);               /* beyond +-8 g saturates */
    CHECK(lis2dh12_read_sample(&d, &s) == LIS2DH12_OK);
    CHECK(s.x == 511 * 16);
}

static void test_fifo(void)
{
    lis2dh12_t d;
    fake_lis_reset();
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, 0x19) == LIS2DH12_OK);
    lis2dh12_config_t c = step_cfg();
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);

    uint8_t level; bool ovr;
    CHECK(lis2dh12_fifo_level(&d, &level, &ovr) == LIS2DH12_OK && level == 0 && !ovr);
    for (int i = 0; i < 20; i++) fake_lis_push_mg(i * 16, -i * 16, 1000);
    CHECK(lis2dh12_fifo_level(&d, &level, &ovr) == LIS2DH12_OK && level == 20 && !ovr);

    lis2dh12_sample_t buf[32];
    size_t n = 0;
    CHECK(lis2dh12_fifo_read(&d, buf, 8, &n) == LIS2DH12_OK && n == 8);   /* limited by max */
    CHECK(buf[0].x == 0 && buf[7].x == 7 * 16 && buf[7].y == -7 * 16 && buf[7].z == 992);
    CHECK(lis2dh12_fifo_read(&d, buf, 32, &n) == LIS2DH12_OK && n == 12);
    CHECK(buf[0].x == 8 * 16 && buf[11].x == 19 * 16);
    CHECK(lis2dh12_fifo_read(&d, buf, 32, &n) == LIS2DH12_OK && n == 0);

    /* Exactly full: 32 samples, reported through OVRN. */
    for (int i = 0; i < 32; i++) fake_lis_push_mg(i * 16, 0, 0);
    CHECK(lis2dh12_fifo_level(&d, &level, &ovr) == LIS2DH12_OK && level == 32 && ovr);
    CHECK(lis2dh12_fifo_read(&d, buf, 32, &n) == LIS2DH12_OK && n == 32);
    CHECK(buf[0].x == 0 && buf[31].x == 31 * 16);

    /* Overflow: oldest samples are lost, the newest 32 remain. */
    for (int i = 0; i < 40; i++) fake_lis_push_mg(i * 16, 0, 0);
    CHECK(lis2dh12_fifo_level(&d, &level, &ovr) == LIS2DH12_OK && level == 32 && ovr);
    CHECK(lis2dh12_fifo_read(&d, buf, 32, &n) == LIS2DH12_OK && n == 32);
    CHECK(buf[0].x == 8 * 16 && buf[31].x == 39 * 16);

    /* Reconfiguring restarts the FIFO empty. */
    for (int i = 0; i < 5; i++) fake_lis_push_mg(0, 0, 0);
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    CHECK(lis2dh12_fifo_level(&d, &level, NULL) == LIS2DH12_OK && level == 0);

    /* Bus error in the middle of a read. */
    for (int i = 0; i < 5; i++) fake_lis_push_mg(0, 0, 0);
    FL.nack = true;
    CHECK(lis2dh12_fifo_read(&d, buf, 32, &n) == LIS2DH12_ERR_BUS && n == 0);
    FL.nack = false;

    /* FIFO read without FIFO configured */
    c.fifo_watermark = 0; c.int1_watermark = false;
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    CHECK(lis2dh12_fifo_read(&d, buf, 32, &n) == LIS2DH12_ERR_PARAM);
}

static void test_wake_and_power_down(void)
{
    lis2dh12_t d;
    fake_lis_reset();
    CHECK(lis2dh12_init(&d, &FAKE_LIS_BUS, 0x19) == LIS2DH12_OK);
    lis2dh12_config_t c = step_cfg();
    CHECK(lis2dh12_configure(&d, &c) == LIS2DH12_OK);
    bool active = true;
    CHECK(lis2dh12_read_wake_source(&d, &active) == LIS2DH12_OK && !active);
    FL.reg[LIS2DH12_INT2_SRC] = 0x60;           /* IA + ZH */
    CHECK(lis2dh12_read_wake_source(&d, &active) == LIS2DH12_OK && active);
    CHECK(lis2dh12_read_wake_source(&d, &active) == LIS2DH12_OK && !active);   /* cleared */

    CHECK(lis2dh12_power_down(&d) == LIS2DH12_OK);
    CHECK(FL.reg[LIS2DH12_CTRL_REG1] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG3] == 0x00);
    CHECK(FL.reg[LIS2DH12_CTRL_REG6] == 0x02);  /* polarity kept: pins stay idle high */
    CHECK(!d.configured);
}

int main(void)
{
    test_init();
    test_configure_registers();
    test_configure_rejects();
    test_conversion();
    test_single_sample();
    test_fifo();
    test_wake_and_power_down();
    printf("lis2dh12: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
