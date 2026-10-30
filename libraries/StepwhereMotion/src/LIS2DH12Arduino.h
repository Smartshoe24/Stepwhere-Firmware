/*
 * LIS2DH12Arduino.h - Connects the plain-C lis2dh12 driver to the Arduino
 * Wire library (STM32duino core).
 *
 * Usage (Stepwhere GSM PCB, IC7):
 *   TwoWire dev_i2c(PA12, PA11);                 // SDA, SCL
 *   static LIS2DH12Arduino accel(dev_i2c, PA1);  // SA0 pin driven HIGH -> 0x19
 *   if (accel.begin() == LIS2DH12_OK) { ... accel.dev() ... }
 *
 * Board wiring traced from the Rev10 Gerbers (docs/STEPS_DECISIONS.md):
 *   IC7 pin 1 SCL  <- PA11, pin 4 SDA <-> PA12 (4.7k pull-ups to the IC7 supply)
 *   IC7 pin 3 SA0  <- PA1 through a 0R resistor
 *   IC7 pin 12 INT1 -> PB3, pin 11 INT2 -> PB4 (no pull-ups; push-pull outputs)
 *   IC7 VDD / VDD_IO / CS on the always-on 3.3 V rail
 */
#ifndef STEPWHERE_LIS2DH12_ARDUINO_H
#define STEPWHERE_LIS2DH12_ARDUINO_H

#include <Arduino.h>
#include <Wire.h>

#include "lis2dh12.h"

#define LIS2DH12_NO_PIN 0xFFFFFFFFUL

class LIS2DH12Arduino {
public:
    /* sa0_pin: MCU pin wired to SA0, driven to sa0_level (LIS2DH12_NO_PIN if
     * SA0 is strapped on the board). The I2C address follows the level. */
    LIS2DH12Arduino(TwoWire &wire, uint32_t sa0_pin = LIS2DH12_NO_PIN, bool sa0_level = true,
                    uint32_t clock_hz = 400000UL)
        : wire_(wire), sa0_(sa0_pin), sa0_level_(sa0_level), clock_hz_(clock_hz) {}

    lis2dh12_status_t begin()
    {
        if (sa0_ != LIS2DH12_NO_PIN) {
            pinMode(sa0_, OUTPUT);
            digitalWrite(sa0_, sa0_level_ ? HIGH : LOW);
        }
        wire_.begin();
        wire_.setClock(clock_hz_);
        /* Datasheet: boot procedure takes 5 ms after power-up. */
        delay(5);
        return lis2dh12_init(&dev_, bus(), address());
    }

    uint8_t address() const { return sa0_level_ ? LIS2DH12_ADDR_SA0_HIGH : LIS2DH12_ADDR_SA0_LOW; }

    /* Try an address without changing the driver state (for bring-up). */
    bool probe(uint8_t addr, uint8_t *who_am_i)
    {
        return read_cb(this, addr, LIS2DH12_WHO_AM_I, who_am_i, 1);
    }

    lis2dh12_t *dev() { return &dev_; }

private:
    const lis2dh12_bus_t *bus()
    {
        bus_.ctx = this;
        bus_.write = &LIS2DH12Arduino::write_cb;
        bus_.read = &LIS2DH12Arduino::read_cb;
        bus_.delay_ms = &LIS2DH12Arduino::delay_cb;
        return &bus_;
    }

    static bool write_cb(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
    {
        LIS2DH12Arduino *self = static_cast<LIS2DH12Arduino *>(ctx);
        self->wire_.beginTransmission(addr);
        self->wire_.write((uint8_t)(len > 1 ? (reg | LIS2DH12_AUTO_INC) : reg));
        for (size_t i = 0; i < len; i++) self->wire_.write(data[i]);
        return self->wire_.endTransmission() == 0;
    }

    static bool read_cb(void *ctx, uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
    {
        LIS2DH12Arduino *self = static_cast<LIS2DH12Arduino *>(ctx);
        self->wire_.beginTransmission(addr);
        self->wire_.write(reg);
        if (self->wire_.endTransmission(false) != 0) return false;   /* repeated start */
        if (self->wire_.requestFrom(addr, (uint8_t)len) != (uint8_t)len) return false;
        for (size_t i = 0; i < len; i++) data[i] = (uint8_t)self->wire_.read();
        return true;
    }

    static void delay_cb(void *, uint32_t ms) { delay(ms); }

    TwoWire       &wire_;
    uint32_t       sa0_;
    bool           sa0_level_;
    uint32_t       clock_hz_;
    lis2dh12_bus_t bus_;
    lis2dh12_t     dev_;
};

#endif /* STEPWHERE_LIS2DH12_ARDUINO_H */
