/*
 * W25QArduino.h - Connects the plain-C w25q driver to the Arduino SPI library
 * (STM32duino core).
 *
 * Usage:
 *   static W25QArduino flash(PA7, PA6, PA5, PA4);   // MOSI, MISO, SCK, CS
 *   if (flash.begin() == W25Q_OK) { ... flash.dev() ... }
 */
#ifndef STEPWHERE_W25Q_ARDUINO_H
#define STEPWHERE_W25Q_ARDUINO_H

#include <Arduino.h>
#include <SPI.h>

#include "w25q.h"

class W25QArduino {
public:
    /* clock_hz: the STM32 core picks the nearest lower prescaler. 16 MHz is
     * well below the 50 MHz limit of the 03h read command. */
    W25QArduino(uint32_t mosi, uint32_t miso, uint32_t sck, uint32_t cs,
                uint32_t clock_hz = 16000000UL)
        : spi_(mosi, miso, sck), cs_(cs), settings_(clock_hz, MSBFIRST, SPI_MODE0) {}

    w25q_status_t begin()
    {
        pinMode(cs_, OUTPUT);
        digitalWrite(cs_, HIGH);
        spi_.begin();
        /* Datasheet 9.3: wait tPUW (5 ms) after power-up before any write. */
        delay(5);

        w25q_bus_t bus;
        bus.ctx = this;
        bus.select = &W25QArduino::select_cb;
        bus.transfer = &W25QArduino::transfer_cb;
        bus.millis = &W25QArduino::millis_cb;
        bus.delay_us = &W25QArduino::delay_us_cb;
        return w25q_init(&dev_, &bus);
    }

    w25q_t *dev() { return &dev_; }

private:
    static void select_cb(void *ctx, bool active)
    {
        W25QArduino *self = static_cast<W25QArduino *>(ctx);
        if (active) {
            self->spi_.beginTransaction(self->settings_);
            digitalWrite(self->cs_, LOW);
        } else {
            digitalWrite(self->cs_, HIGH);
            self->spi_.endTransaction();
        }
    }

    static void transfer_cb(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
    {
        W25QArduino *self = static_cast<W25QArduino *>(ctx);
        for (size_t i = 0; i < len; i++) {
            uint8_t b = self->spi_.transfer(tx ? tx[i] : 0xFF);
            if (rx) rx[i] = b;
        }
    }

    static uint32_t millis_cb(void *) { return millis(); }
    static void delay_us_cb(void *, uint32_t us) { delayMicroseconds(us); }

    SPIClass    spi_;
    uint32_t    cs_;
    SPISettings settings_;
    w25q_t      dev_;
};

#endif /* STEPWHERE_W25Q_ARDUINO_H */
