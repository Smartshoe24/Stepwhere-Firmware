/*
 * lis2dh12.c - see lis2dh12.h.
 */
#include "lis2dh12.h"

/* Sensitivity in mg/digit after right-aligning the output (datasheet
 * table 4), indexed [mode][fs]. */
static const uint8_t k_sens_mg[3][4] = {
    /* 2g  4g  8g  16g */
    { 16, 32, 64, 192 },  /* low-power, 8-bit  */
    {  4,  8, 16,  48 },  /* normal,   10-bit  */
    {  1,  2,  4,  12 },  /* high-res, 12-bit  */
};
/* Output words are left-justified: shift to drop the unused low bits. */
static const uint8_t k_shift[3] = { 8, 6, 4 };
/* INTx_THS LSB in mg per full scale (datasheet 8.23). */
static const uint8_t k_ths_lsb_mg[4] = { 16, 32, 62, 186 };

static lis2dh12_status_t wr(lis2dh12_t *dev, uint8_t reg, uint8_t val)
{
    return dev->bus.write(dev->bus.ctx, dev->addr, reg, &val, 1) ? LIS2DH12_OK : LIS2DH12_ERR_BUS;
}

static lis2dh12_status_t rd(lis2dh12_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if (len > 1) reg |= LIS2DH12_AUTO_INC;
    return dev->bus.read(dev->bus.ctx, dev->addr, reg, buf, len) ? LIS2DH12_OK : LIS2DH12_ERR_BUS;
}

#define TRY(expr) do { lis2dh12_status_t st_ = (expr); if (st_ != LIS2DH12_OK) return st_; } while (0)

uint16_t lis2dh12_odr_hz(lis2dh12_odr_t odr)
{
    static const uint16_t hz[8] = { 0, 1, 10, 25, 50, 100, 200, 400 };
    return ((unsigned)odr < 8u) ? hz[odr] : 0;
}

int16_t lis2dh12_raw_to_mg(int16_t raw, lis2dh12_mode_t mode, lis2dh12_fs_t fs)
{
    if ((unsigned)mode > 2u || (unsigned)fs > 3u) return 0;
    /* Arithmetic shift of a negative value: divide instead to stay portable. */
    int32_t v = raw;
    int32_t div = (int32_t)1 << k_shift[mode];
    v = (v >= 0) ? v / div : -((-v) / div);
    v *= k_sens_mg[mode][fs];
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

lis2dh12_status_t lis2dh12_read_id(lis2dh12_t *dev, uint8_t *id)
{
    if (!dev || !id) return LIS2DH12_ERR_PARAM;
    return rd(dev, LIS2DH12_WHO_AM_I, id, 1);
}

lis2dh12_status_t lis2dh12_init(lis2dh12_t *dev, const lis2dh12_bus_t *bus, uint8_t addr)
{
    if (!dev || !bus || !bus->write || !bus->read || !bus->delay_ms) return LIS2DH12_ERR_PARAM;
    dev->bus = *bus;
    dev->addr = addr;
    dev->configured = false;

    uint8_t id = 0;
    TRY(lis2dh12_read_id(dev, &id));
    if (id != LIS2DH12_WHO_AM_I_VALUE) return LIS2DH12_ERR_ID;

    /* Reload trimming values, as after a power cycle. The MCU may have been
     * reset while the sensor kept running with an old configuration. */
    TRY(wr(dev, LIS2DH12_CTRL_REG5, LIS2DH12_CTRL5_BOOT));
    dev->bus.delay_ms(dev->bus.ctx, 5);

    /* Known state: powered down, no interrupts, FIFO bypass. */
    TRY(wr(dev, LIS2DH12_CTRL_REG1, 0x00));
    TRY(wr(dev, LIS2DH12_CTRL_REG3, 0x00));
    TRY(wr(dev, LIS2DH12_CTRL_REG6, 0x00));
    TRY(wr(dev, LIS2DH12_INT1_CFG, 0x00));
    TRY(wr(dev, LIS2DH12_INT2_CFG, 0x00));
    TRY(wr(dev, LIS2DH12_CLICK_CFG, 0x00));
    TRY(wr(dev, LIS2DH12_FIFO_CTRL_REG, LIS2DH12_FIFO_MODE_BYPASS));
    TRY(wr(dev, LIS2DH12_CTRL_REG5, 0x00));
    TRY(wr(dev, LIS2DH12_CTRL_REG0, LIS2DH12_CTRL0_DEFAULT));
    TRY(wr(dev, LIS2DH12_TEMP_CFG_REG, 0x00));
    return LIS2DH12_OK;
}

lis2dh12_status_t lis2dh12_configure(lis2dh12_t *dev, const lis2dh12_config_t *cfg)
{
    if (!dev || !cfg) return LIS2DH12_ERR_PARAM;
    if ((unsigned)cfg->odr > LIS2DH12_ODR_400HZ || (unsigned)cfg->mode > LIS2DH12_MODE_HIGH_RES ||
        (unsigned)cfg->fs > LIS2DH12_FS_16G || cfg->fifo_watermark > 31u ||
        (cfg->int1_watermark && cfg->fifo_watermark == 0) ||
        (cfg->wake_threshold_mg && cfg->odr == LIS2DH12_ODR_OFF) ||
        cfg->wake_duration > 127u)
        return LIS2DH12_ERR_PARAM;

    const uint8_t pol = cfg->int_active_low ? LIS2DH12_CTRL6_INT_POL_LOW : 0;

    /* 1. Interrupts off and sensor stopped while reconfiguring. */
    TRY(wr(dev, LIS2DH12_CTRL_REG3, 0x00));
    TRY(wr(dev, LIS2DH12_CTRL_REG6, pol));
    TRY(wr(dev, LIS2DH12_CTRL_REG1, 0x00));

    /* 2. Full scale, resolution. BDU keeps the high and low bytes of one
     *    sample together. */
    uint8_t c4 = LIS2DH12_CTRL4_BDU | (uint8_t)(cfg->fs << 4);
    if (cfg->mode == LIS2DH12_MODE_HIGH_RES) c4 |= LIS2DH12_CTRL4_HR;
    TRY(wr(dev, LIS2DH12_CTRL_REG4, c4));

    /* 3. High-pass filter only on the INT2 motion detector, never on the
     *    data (the step counter needs gravity in the samples). */
    TRY(wr(dev, LIS2DH12_CTRL_REG2, cfg->wake_threshold_mg ? LIS2DH12_CTRL2_HP_IA2 : 0x00));

    /* 4. FIFO: going through bypass empties it. */
    TRY(wr(dev, LIS2DH12_FIFO_CTRL_REG, LIS2DH12_FIFO_MODE_BYPASS));
    if (cfg->fifo_watermark) {
        TRY(wr(dev, LIS2DH12_CTRL_REG5, LIS2DH12_CTRL5_FIFO_EN));
        TRY(wr(dev, LIS2DH12_FIFO_CTRL_REG, LIS2DH12_FIFO_MODE_STREAM | cfg->fifo_watermark));
    } else {
        TRY(wr(dev, LIS2DH12_CTRL_REG5, 0x00));
    }

    /* 5. Motion detector on interrupt generator 2. */
    if (cfg->wake_threshold_mg) {
        uint8_t lsb = k_ths_lsb_mg[cfg->fs];
        uint32_t ths = ((uint32_t)cfg->wake_threshold_mg + lsb - 1u) / lsb;
        if (ths < 1u) ths = 1u;
        if (ths > 127u) ths = 127u;
        TRY(wr(dev, LIS2DH12_INT2_THS, (uint8_t)ths));
        TRY(wr(dev, LIS2DH12_INT2_DURATION, cfg->wake_duration));
        TRY(wr(dev, LIS2DH12_INT2_CFG, LIS2DH12_INT_CFG_XYZ_HIGH));
    } else {
        TRY(wr(dev, LIS2DH12_INT2_CFG, 0x00));
    }

    /* 6. Start measuring. */
    uint8_t c1 = (uint8_t)(cfg->odr << 4);
    if (cfg->odr != LIS2DH12_ODR_OFF) c1 |= LIS2DH12_CTRL1_XYZ_EN;
    if (cfg->mode == LIS2DH12_MODE_LOW_POWER) c1 |= LIS2DH12_CTRL1_LPEN;
    TRY(wr(dev, LIS2DH12_CTRL_REG1, c1));

    /* 7. Reading REFERENCE sets the high-pass filter to the current value,
     *    so gravity does not trigger INT2 at start. Then clear INT2_SRC. */
    uint8_t dummy;
    TRY(rd(dev, LIS2DH12_REFERENCE, &dummy, 1));
    TRY(rd(dev, LIS2DH12_INT2_SRC, &dummy, 1));

    /* 8. Route interrupts last. */
    TRY(wr(dev, LIS2DH12_CTRL_REG3, cfg->int1_watermark ? LIS2DH12_CTRL3_I1_WTM : 0x00));
    TRY(wr(dev, LIS2DH12_CTRL_REG6, (uint8_t)(pol | (cfg->wake_threshold_mg ? LIS2DH12_CTRL6_I2_IA2 : 0))));

    dev->cfg = *cfg;
    dev->configured = true;
    return LIS2DH12_OK;
}

static void decode(const lis2dh12_t *dev, const uint8_t b[6], lis2dh12_sample_t *out)
{
    out->x = lis2dh12_raw_to_mg((int16_t)(uint16_t)(b[0] | (b[1] << 8)), dev->cfg.mode, dev->cfg.fs);
    out->y = lis2dh12_raw_to_mg((int16_t)(uint16_t)(b[2] | (b[3] << 8)), dev->cfg.mode, dev->cfg.fs);
    out->z = lis2dh12_raw_to_mg((int16_t)(uint16_t)(b[4] | (b[5] << 8)), dev->cfg.mode, dev->cfg.fs);
}

lis2dh12_status_t lis2dh12_read_sample(lis2dh12_t *dev, lis2dh12_sample_t *out)
{
    if (!dev || !out || !dev->configured) return LIS2DH12_ERR_PARAM;
    uint8_t b[6];
    TRY(rd(dev, LIS2DH12_OUT_X_L, b, 6));
    decode(dev, b, out);
    return LIS2DH12_OK;
}

lis2dh12_status_t lis2dh12_fifo_level(lis2dh12_t *dev, uint8_t *count, bool *overrun)
{
    if (!dev || !count) return LIS2DH12_ERR_PARAM;
    uint8_t src;
    TRY(rd(dev, LIS2DH12_FIFO_SRC_REG, &src, 1));
    bool ovr = (src & LIS2DH12_FIFO_SRC_OVRN) != 0;
    /* FSS has 5 bits (0..31); OVRN_FIFO is set when all 32 slots are full. */
    *count = ovr ? (uint8_t)LIS2DH12_FIFO_DEPTH : (uint8_t)(src & LIS2DH12_FIFO_SRC_FSS);
    if (overrun) *overrun = ovr;
    return LIS2DH12_OK;
}

lis2dh12_status_t lis2dh12_fifo_read(lis2dh12_t *dev, lis2dh12_sample_t *buf, size_t max, size_t *n)
{
    if (!dev || !buf || !n || !dev->configured || dev->cfg.fifo_watermark == 0) return LIS2DH12_ERR_PARAM;
    *n = 0;
    uint8_t level;
    TRY(lis2dh12_fifo_level(dev, &level, NULL));
    size_t todo = level < max ? level : max;
    /* One 6-byte read per sample: each read pops one FIFO slot and stays
     * well inside the 32-byte Wire buffer. */
    for (size_t i = 0; i < todo; i++) {
        uint8_t b[6];
        TRY(rd(dev, LIS2DH12_OUT_X_L, b, 6));
        decode(dev, b, &buf[i]);
        *n = i + 1;
    }
    return LIS2DH12_OK;
}

lis2dh12_status_t lis2dh12_read_wake_source(lis2dh12_t *dev, bool *active)
{
    if (!dev || !active) return LIS2DH12_ERR_PARAM;
    uint8_t src;
    TRY(rd(dev, LIS2DH12_INT2_SRC, &src, 1));
    *active = (src & LIS2DH12_INT_SRC_IA) != 0;
    return LIS2DH12_OK;
}

lis2dh12_status_t lis2dh12_power_down(lis2dh12_t *dev)
{
    if (!dev) return LIS2DH12_ERR_PARAM;
    TRY(wr(dev, LIS2DH12_CTRL_REG3, 0x00));
    TRY(wr(dev, LIS2DH12_CTRL_REG6, dev->cfg.int_active_low ? LIS2DH12_CTRL6_INT_POL_LOW : 0));
    TRY(wr(dev, LIS2DH12_CTRL_REG1, 0x00));
    dev->configured = false;
    return LIS2DH12_OK;
}

const char *lis2dh12_status_str(lis2dh12_status_t st)
{
    switch (st) {
    case LIS2DH12_OK:        return "OK";
    case LIS2DH12_ERR_PARAM: return "bad parameter";
    case LIS2DH12_ERR_BUS:   return "I2C error (no ACK)";
    case LIS2DH12_ERR_ID:    return "WHO_AM_I mismatch (not a LIS2DH12)";
    }
    return "unknown";
}
