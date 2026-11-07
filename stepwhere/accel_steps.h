/*
 * accel_steps.h - LIS2DH12 accelerometer + software step counter for the
 * Stepwhere firmware.
 *
 * The sensor samples at 25 Hz into its 32-sample FIFO (1.28 s of data). A
 * hardware timer (TIM17) reads the FIFO every 500 ms from its interrupt,
 * so steps keep being counted while loop() is blocked in long GSM/GPS
 * waits. For every detected step it calls accel_steps_on_steps(n), which
 * the sketch implements (the old INT2-per-step ISR logic).
 *
 * Pins (Rev10 Gerbers, confirmed on a Rev11 board with lis2dh12_bringup):
 *   SDA PA12, SCL PA11, SA0 PA1 (driven HIGH -> address 0x19).
 * INT1/INT2 (PB3/PB4) are not used yet; INT2 is for wake-from-sleep later.
 */
#ifndef STEPWHERE_ACCEL_STEPS_H
#define STEPWHERE_ACCEL_STEPS_H

#include <Arduino.h>
#include <Wire.h>

/* Implemented by the sketch. Called from the TIM17 interrupt. */
void accel_steps_on_steps(uint32_t steps);

/* Set up the sensor and start the 500 ms service timer.
 * Returns false if the LIS2DH12 does not answer (no steps are counted;
 * the timer keeps retrying every 5 s). */
bool accel_steps_begin(TwoWire &wire, uint32_t sa0_pin);

/* One status line: sensor state, steps counted, errors. Call from loop(). */
void accel_steps_print_status(Print &out);

/* Prints the interrupt priorities the FIFO read depends on and returns
 * false if they are wrong (see accel_steps.cpp). Call once after begin(). */
bool accel_steps_check_irq_priorities(Print &out);

/* STEPS_DEBUG 1: the TIM17 interrupt also collects step counter statistics
 * (bursts, candidates, reject reasons, max |a|, gravity estimate), and
 * accel_steps_print_debug() prints them as one [STEPS] line and starts a
 * new period. Costs one integer square root per sample and ~60 bytes of
 * RAM. Set to 0 to remove it. */
#ifndef STEPS_DEBUG
#define STEPS_DEBUG 1
#endif

#if STEPS_DEBUG
void accel_steps_print_debug(Print &out);
#endif

#endif
