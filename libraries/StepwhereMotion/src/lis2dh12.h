/*
 * lis2dh12.h - Minimal driver for the ST LIS2DH12 3-axis accelerometer (I2C).
 *
 * Plain C with no Arduino dependency, so it runs in the firmware (Arduino
 * Wire, see LIS2DH12Arduino.h) and in the host tests (tests/motion_host).
 * The caller provides the I2C transport as two function pointers.
 *
 * Covers what the step counter needs: output data rate / mode / full scale,
 * FIFO in stream mode with a watermark interrupt on INT1, and a motion
 * (wake-up) interrupt on INT2. Register numbers are from the LIS2DH12
 * datasheet (DocID025056), section 8.
 */
#ifndef STEPWHERE_LIS2DH12_H
#define STEPWHERE_LIS2DH12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIS2DH12_ADDR_SA0_HIGH  0x19u   /* SA0 = 1 (Stepwhere board: PA1 HIGH) */
#define LIS2DH12_ADDR_SA0_LOW   0x18u
#define LIS2DH12_WHO_AM_I_VALUE 0x33u
#define LIS2DH12_FIFO_DEPTH     32u

/* Registers */
#define LIS2DH12_STATUS_REG_AUX 0x07u
#define LIS2DH12_OUT_TEMP_L     0x0Cu
#define LIS2DH12_WHO_AM_I       0x0Fu
#define LIS2DH12_CTRL_REG0      0x1Eu
#define LIS2DH12_TEMP_CFG_REG   0x1Fu
#define LIS2DH12_CTRL_REG1      0x20u
#define LIS2DH12_CTRL_REG2      0x21u
#define LIS2DH12_CTRL_REG3      0x22u
#define LIS2DH12_CTRL_REG4      0x23u
#define LIS2DH12_CTRL_REG5      0x24u
#define LIS2DH12_CTRL_REG6      0x25u
#define LIS2DH12_REFERENCE      0x26u
#define LIS2DH12_STATUS_REG     0x27u
#define LIS2DH12_OUT_X_L        0x28u
#define LIS2DH12_FIFO_CTRL_REG  0x2Eu
#define LIS2DH12_FIFO_SRC_REG   0x2Fu
#define LIS2DH12_INT1_CFG       0x30u
#define LIS2DH12_INT1_SRC       0x31u
#define LIS2DH12_INT1_THS       0x32u
#define LIS2DH12_INT1_DURATION  0x33u
#define LIS2DH12_INT2_CFG       0x34u
#define LIS2DH12_INT2_SRC       0x35u
#define LIS2DH12_INT2_THS       0x36u
#define LIS2DH12_INT2_DURATION  0x37u
#define LIS2DH12_CLICK_CFG      0x38u
#define LIS2DH12_ACT_THS        0x3Eu
#define LIS2DH12_ACT_DUR        0x3Fu

/* Register bits used by the driver */
#define LIS2DH12_CTRL0_DEFAULT  0x10u   /* bits 0-6 must keep this value      */
#define LIS2DH12_CTRL1_LPEN     0x08u
#define LIS2DH12_CTRL1_XYZ_EN   0x07u
#define LIS2DH12_CTRL2_HP_IA2   0x02u
#define LIS2DH12_CTRL3_I1_WTM   0x04u
#define LIS2DH12_CTRL4_BDU      0x80u
#define LIS2DH12_CTRL4_HR       0x08u
#define LIS2DH12_CTRL5_BOOT     0x80u
#define LIS2DH12_CTRL5_FIFO_EN  0x40u
#define LIS2DH12_CTRL6_I2_IA2   0x20u
#define LIS2DH12_CTRL6_INT_POL_LOW 0x02u
#define LIS2DH12_FIFO_MODE_BYPASS 0x00u
#define LIS2DH12_FIFO_MODE_STREAM 0x80u
#define LIS2DH12_FIFO_SRC_WTM   0x80u
#define LIS2DH12_FIFO_SRC_OVRN  0x40u
#define LIS2DH12_FIFO_SRC_EMPTY 0x20u
#define LIS2DH12_FIFO_SRC_FSS   0x1Fu
#define LIS2DH12_INT_CFG_XYZ_HIGH 0x2Au /* OR of XHIE, YHIE, ZHIE             */
#define LIS2DH12_INT_SRC_IA     0x40u
#define LIS2DH12_AUTO_INC       0x80u   /* sub-address MSB: auto-increment    */

typedef enum {
    LIS2DH12_OK = 0,
    LIS2DH12_ERR_PARAM,   /* bad argument or unsupported setting            */
    LIS2DH12_ERR_BUS,     /* I2C transfer failed (NACK, bus stuck)          */
    LIS2DH12_ERR_ID       /* WHO_AM_I is not 0x33: wrong chip or address    */
} lis2dh12_status_t;

/* Output data rates (CTRL_REG1 ODR[3:0]). */
typedef enum {
    LIS2DH12_ODR_OFF = 0,
    LIS2DH12_ODR_1HZ = 1,
    LIS2DH12_ODR_10HZ = 2,
    LIS2DH12_ODR_25HZ = 3,
    LIS2DH12_ODR_50HZ = 4,
    LIS2DH12_ODR_100HZ = 5,
    LIS2DH12_ODR_200HZ = 6,
    LIS2DH12_ODR_400HZ = 7
} lis2dh12_odr_t;

/* Resolution modes: 8-bit low-power, 10-bit normal, 12-bit high-resolution. */
typedef enum {
    LIS2DH12_MODE_LOW_POWER = 0,
    LIS2DH12_MODE_NORMAL,
    LIS2DH12_MODE_HIGH_RES
} lis2dh12_mode_t;

typedef enum {
    LIS2DH12_FS_2G = 0,
    LIS2DH12_FS_4G = 1,
    LIS2DH12_FS_8G = 2,
    LIS2DH12_FS_16G = 3
} lis2dh12_fs_t;

/* I2C transport supplied by the caller. ctx is passed back unchanged.
 * Both return true on success. For len > 1 the driver already sets the
 * auto-increment bit in reg. */
typedef struct {
    void *ctx;
    bool (*write)(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);
    bool (*read)(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
    void (*delay_ms)(void *ctx, uint32_t ms);
} lis2dh12_bus_t;

typedef struct {
    lis2dh12_odr_t  odr;
    lis2dh12_mode_t mode;
    lis2dh12_fs_t   fs;
    /* FIFO in stream mode. 0 = FIFO off (read single samples).
     * 1..31 = FIFO on, watermark level in samples. */
    uint8_t         fifo_watermark;
    /* Route the FIFO watermark to INT1. */
    bool            int1_watermark;
    /* Motion (wake-up) interrupt on INT2: fires while the high-pass filtered
     * acceleration on any axis exceeds the threshold. 0 = off. */
    uint16_t        wake_threshold_mg;
    /* Motion must last this many samples (1/ODR each) before INT2 fires. */
    uint8_t         wake_duration;
    /* INT pins are push-pull; true = active low (idle high). */
    bool            int_active_low;
} lis2dh12_config_t;

typedef struct {
    int16_t x, y, z;      /* milli-g */
} lis2dh12_sample_t;

typedef struct {
    lis2dh12_bus_t  bus;
    uint8_t         addr;
    lis2dh12_config_t cfg;
    bool            configured;
} lis2dh12_t;

/* Reboot the chip's trimming memory, check WHO_AM_I and leave it powered
 * down with all interrupts off. */
lis2dh12_status_t lis2dh12_init(lis2dh12_t *dev, const lis2dh12_bus_t *bus, uint8_t addr);

/* Apply a configuration. Interrupts are routed only after the rest is set,
 * and the FIFO is restarted empty. */
lis2dh12_status_t lis2dh12_configure(lis2dh12_t *dev, const lis2dh12_config_t *cfg);

/* Read WHO_AM_I into *id (any value, no check). */
lis2dh12_status_t lis2dh12_read_id(lis2dh12_t *dev, uint8_t *id);

/* Read the newest sample (FIFO off) or the oldest unread one (FIFO on). */
lis2dh12_status_t lis2dh12_read_sample(lis2dh12_t *dev, lis2dh12_sample_t *out);

/* Number of unread samples in the FIFO (0..32). *overrun is set when the
 * FIFO is full, so the oldest samples are being overwritten and data was
 * probably lost (read faster); may be NULL. */
lis2dh12_status_t lis2dh12_fifo_level(lis2dh12_t *dev, uint8_t *count, bool *overrun);

/* Read up to max samples from the FIFO. *n gets the number read. */
lis2dh12_status_t lis2dh12_fifo_read(lis2dh12_t *dev, lis2dh12_sample_t *buf, size_t max, size_t *n);

/* Read and clear INT2_SRC. *active = motion interrupt condition present. */
lis2dh12_status_t lis2dh12_read_wake_source(lis2dh12_t *dev, bool *active);

/* Stop measuring (about 0.5 uA). lis2dh12_configure() starts it again. */
lis2dh12_status_t lis2dh12_power_down(lis2dh12_t *dev);

/* Convert one raw left-justified output word to milli-g. */
int16_t lis2dh12_raw_to_mg(int16_t raw, lis2dh12_mode_t mode, lis2dh12_fs_t fs);

/* Samples per second for an ODR code (0 for OFF). */
uint16_t lis2dh12_odr_hz(lis2dh12_odr_t odr);

const char *lis2dh12_status_str(lis2dh12_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* STEPWHERE_LIS2DH12_H */
