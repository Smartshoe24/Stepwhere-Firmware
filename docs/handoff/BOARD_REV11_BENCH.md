# Bench board (Rev11): known faults and what to measure

Board used for all OTA and QA tests in Nov 2026.

| | |
|---|---|
| IMEI | 869938082392680 |
| STM32 chip UID | 0051006C3434510231303739 |
| SIM ICCID | 89917099419000955651 |
| Modem | N706B-A07-STD-BZ_CN1X-008 |
| Marking | STEPWHERE R11 16.08.2026 |
| QA unit (admin) | panel.stepwhere.in/qa/units/2a3d8192-7f9e-435e-a99f-68c3b4c604b4 |
| Customer binding | **unbound** for QA tests (re-bind to the child to use it as a tracker) |
| Firmware on it | QA firmware qa-0.7.2 (to go back: flash `stepwhere_factory.hex` of v2.2.0, full chip erase) |

Battery: 1500 mAh Li-ion, 2.8–4.2 V. Battery wires are soldered straight onto the
BAT and 5V pads (bottom left of the modem side).

## Faults

1. **Battery sense broken (PB10 reads 4095 = saturated, "6.59 V").** Read correctly
   on 2026-10-31 (3.87 V, raw 2397), wrong since 2026-11-03
   (`bringup/rev11` branch, `docs/BRINGUP_REV11.md` T2, T9). PB10 is an MCU pin; the
   fault is in the 1:2 divider: most likely the lower resistor (PB10 → GND) open,
   lifted or broken, or PB10 touching 3.3 V / the battery. Effect: firmware thinks
   the battery is always full, the low-battery cut-off never works.
   *Measure (battery on):* divider middle point to GND = half the battery voltage?
   *(battery off):* both divider resistors, same value?
   Suspect area: the heavy rework at the BAT pads (big solder blobs, loose wire
   strands, burnt flux). Still to confirm: what the red wire on the 5V pad goes to.
2. **LIS2DH12 does not answer on I2C** (0x19/0x18/0x1D/0x1E) in every QA run; once
   (qa-0.7.1) it held the bus and froze the run (qa-0.7.2 has a watchdog for this).
   Production firmware: "LIS2DH12 not responding", 0 steps.
   *Measure:* sensor supply ≈ 3.3 V; SDA PA12 / SCL PA11 and pull-ups R10/R23
   (4.7 k), SA0 via R26 (PA1). The chip's pads are under it (LGA): photos cannot
   show the joints.
3. **GPS: only 1–3 satellites, no fix** – indoors; the module itself works (NMEA OK).
4. **Weak battery connection:** several brown-out resets every time the battery is
   connected (connector bounce).
5. A light-brown spot on the middle chip on the MCU side (photo 2026-11-15): scratch,
   flux or heat? Look closely.

Working: modem, SIM, LTE (RSRP −96 dBm), data, Wi-Fi scan (5–9 APs), serving cell,
W25Q128 (EF4018, write test OK), uploads to the server (10/10 HTTP 201).

## Needed to pin the faults
The **R11 assembly drawing / schematic** (battery-sense resistors on PB10, the
accelerometer section): the board has no reference designators printed.
