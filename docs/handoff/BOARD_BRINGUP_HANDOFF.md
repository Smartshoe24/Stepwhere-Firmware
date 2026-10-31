# Handoff: first bring-up of the newly assembled GSM PCB (Rev11, LIS2DH12)

Context for a new chat that tests a newly assembled board step by step and
finds out why the firmware that worked for months does not work on it.
Written 2026-10-31 from the LIS2DH12 step-counter chat. **VERIFIED** = seen on
this board in the debug log; **UNKNOWN** = not tested yet.

## 1. The situation

* The board is a **new assembly, Rev11** (assembly drawing "R11 STEPWHERE
  16.08.2026"). This is the first test after assembly.
* The firmware that worked for months on the **old boards** (v1.2, LIS2DS12
  accelerometer) does **not** work on this board. The exact symptom is not
  written down yet. **Ask the owner first** what happens: no output at all,
  stuck at modem/SIM/GPRS, no GPS fix, resets, wrong battery reading, etc.
* Known hardware change: accelerometer IC7 is **LIS2DH12** instead of
  LIS2DS12 (no hardware pedometer, address 0x19 instead of 0x1D). The old
  firmware's accelerometer code cannot work on it; that alone gives "no steps",
  not a dead board. Other changes between the old boards and Rev11 are
  **UNKNOWN**: ask.

## 2. Rules

* Repo `Smartshoe24/Stepwhere-Firmware`. Branches:
  * `main`: only `stepwhere.zip` (original v1.2 sketch).
  * `ota/foundation` (PR #1, OTA work): **do not touch**.
  * `feature/lis2dh12-steps` (PR #2): LIS2DH12 driver, step counter,
    bring-up sketch, recordings, `stepwhere/stepwhere.ino` changed to LIS2DH12.
  * `Hardware--Stepwhere`: Rev10 Gerber files (+ a stray CSV).
  * For bring-up work use a new branch, e.g. `bringup/rev11` from
    `feature/lis2dh12-steps`.
* Owner: Mac, Terminal (zsh). Not a developer for every detail: give
  **numbered, simple steps** and explain technical words. Always give the full
  command including `cd ~/stepwhere`.
* Toolchain: Arduino / STM32duino core **2.x** (tested 2.12.0), board
  "Generic STM32G0 series", part "Generic G070CBTx". Core 3.0.0 does **not**
  build the sketch. Libraries: TinyGPSPlus 1.0.3, STM32_TimerInterrupt 1.3.0,
  STM32duino Low Power 2.0.0, STM32duino RTC 2.0.0, plus `libraries/` from the repo.
* Flashing: STM32CubeProgrammer app + ST-LINK (SWD) at **0x08000000**
  (no bootloader on this board now). The CLI is not in PATH; full path:
  `/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI`.
* Debug UART: CP2102 adapter, `/dev/cu.usbserial-0001`, 115200 8N1.
  Tools in `~/stepwhere` on the Mac (downloaded, no repo clone):
  * `python3 ota_uart.py monitor --port /dev/cu.usbserial-0001`
    (keys typed are sent to the board; Ctrl-C quits)
  * `python3 ota_uart.py ports`
  * `python3 record_steps.py dump|live --port ...`
  * Only one program can use the port ("Resource busy" = close the other one,
    `lsof /dev/cu.usbserial-0001`, `kill <PID>`).
  * Download files: `curl -LO https://github.com/Smartshoe24/Stepwhere-Firmware/raw/<branch>/<path>`
* Uploads from the owner (logs, CSV, photos): GitHub web "Add file > Upload
  files"; check the branch selector first (files have landed on the wrong
  branch before).

## 3. Hardware (GSM PCB)

BOM (owner's spreadsheet "BOM-GSM Neoway"):

| Ref | Part | Function |
|---|---|---|
| IC4 | STM32G070CBT6 (LQFP48; BOM says G071/G070, **G070 fitted**) | MCU, 128 KB flash, 36 KB RAM |
| IC6 | Neoway N706B | GSM/LTE modem |
| IC7 | LIS2DH12 (LGA-12) | accelerometer |
| IC8 | W25Q128 | 16 MB SPI flash (OTA, logs) |
| IC1 | TXB0102 | 2-ch level shifter (path **UNKNOWN**; not in the accelerometer path) |
| IC2 | TP4056 | Li-ion charger (CHRG/STDBY open-drain) |
| IC3 | AP22811 | P-channel load switch (what it switches: **UNKNOWN**) |
| IC5 | AP7365-33 | 3.3 V LDO, 600 mA |
| J1 | 4-pin 1.0 mm FFC | (probably GPS module G7A) |
| J3 | Nano SIM | |
| Q1-Q5 | MMBT2222A | NPN drivers (PWRKEY, LEDs, ...) |
| D2/D4 | green / yellow LED | (driving pins **UNKNOWN**) |
| D3 | 5.1 V zener | |

Pins used by the firmware (`stepwhere/stepwhere.ino`):

| Function | Pins |
|---|---|
| Debug console USART1 | PB6 TX, PB7 RX, 115200 |
| GSM N706B USART2 | PA2 TX, PA3 RX, 115200; PWRKEY **PB13** (via NPN), GSM/GPS LDO enable **PA10** |
| GPS G7A USART3 | PB8 TX, PB9 RX, 9600 |
| Battery ADC | PB10 (ADC_IN8), internal VREF; MIN_BAT_VOLT 3.2 V |
| Charger status | PB14 CHRG, PB15 STDBY (TP4056, active low, MCU pull-ups) |
| Accelerometer I2C | PA12 SDA, PA11 SCL, SA0 PA1 (HIGH -> 0x19); INT1 PB3, INT2 PB4 |
| External flash SPI1 | PA5 SCK, PA6 MISO, PA7 MOSI, /CS PC6, /WP PC7, /HOLD PB12 (driven high) |

Accelerometer wiring traced from the **Rev10** Gerbers (`Hardware--Stepwhere`);
Rev11 Gerbers were not available. The pins above are confirmed on the Rev11
board by the bring-up firmware.

Open hardware items from earlier chats:

* Series part between the 3.3 V rail and the accelerometer supply (net
  "3.3VL", bottom side ~3 mm from IC8 pin 1, not on the BOM): value
  **UNKNOWN**. 0 R / ferrite / <= 100 R fine; 1 k would be a problem.
* LIS2DH12 pin 5 is floating (reserved pin; tie to GND next revision).
* Old boards ran the LIS2DS12 (1.62-1.98 V part) on 3.3 V, outside its range.
* "Unexplained resets on the bench board" were noted in the OTA chat.

## 4. What is VERIFIED on this Rev11 board (2026-10-31)

With `prebuilt/lis2dh12_bringup.bin` (branch `feature/lis2dh12-steps`):

| Check | Result |
|---|---|
| MCU runs, SWD flashing works, debug UART TX/RX work | VERIFIED |
| I2C bus, LIS2DH12 WHO_AM_I 0x33 at 0x19 and 0x18 (SA0 follows PA1) | VERIFIED |
| Accelerometer data, 50 Hz FIFO (~52 Hz real), |a| at rest ~1000 mg | VERIFIED |
| INT1 (PB3) | VERIFIED |
| INT2 (PB4) | 1 pass, 1 fail (weak movement?) - recheck |
| W25Q128 JEDEC ID EF 4018, write/read of the motion log | VERIFIED |
| Step counter: 192 of 202 steps walking, 0 while sitting | VERIFIED (bring-up firmware) |
| Power: board reset ~40 times while being moved; reset cause POWER/brown-out (RCC_CSR 0x0C); fixed by powering from the battery and pressing the connector in | VERIFIED: the power connection is mechanically weak |
| Power from battery, not moved: 10+ min without reset | VERIFIED |

**Not tested at all on this board: GSM modem (power-on, SIM, network, GPRS,
HTTP), GPS module, battery ADC reading, charger status pins, GSM/GPS LDO
(PA10), LEDs, current consumption.** These are the most likely places for the
problem, because the old firmware spends its first minutes there.

Note: the debug output of `lis2dh12_bringup` never showed the GSM LDO turned on
(it keeps PA10 LOW), so the modem side has had no power yet in any test.

## 5. Firmware available

| File | Branch | What it is |
|---|---|---|
| `prebuilt/lis2dh12_bringup.bin` (v0.5) | feature/lis2dh12-steps | accelerometer + flash check, motion recorder (keys i d n x e t r h) |
| `prebuilt/stepwhere_v1.2-lis2dh12.bin` | feature/lis2dh12-steps | main firmware v1.2 with LIS2DH12 step counter (accel_steps.cpp, TIM17 500 ms), no OTA, 0x08000000. **Not yet run on hardware** |
| `prebuilt/modem_probe.bin` | ota/foundation | N706B test: key `n` = power on, network, downloads; `m` = AT pass-through (`~~~` leaves). Good first GSM test |
| `prebuilt/flash_selftest.bin` | ota/foundation | W25Q128 erase/write/read test (last 64 KB only) |
| `stepwhere.zip` (v1.2 original) | main | the firmware that worked on the old boards |

Main firmware start-up order (setup): ADC init, debug UART, charger status
print, ADC print, **LDO on (PA10)**, GSM/GPS UARTs, accelerometer init, minute
timer, `gsm_power_on()` (PWRKEY pulse), then up to 5 retries of: modem check
(AT/SIM), IMEI (15 digits), GPRS (`AT+CGDCONT` APN "internet", `AT+XIIC=1`).
loop(): charge status, ADC, GPS feed, HTTP POST to
`http://ingest.stepwhere.in/location`. GPS no-fix for 10 min ->
`NVIC_SystemReset()`. Deep sleep is disabled (`#if 0`).

## 6. Suggested step-by-step plan

1. Ask for the exact symptom with the old firmware and its full debug log.
2. Visual check / photos of both sides (solder bridges on IC4 LQFP48 and IC6
   N706B, rotated parts, missing parts vs BOM, battery connector).
3. Power: measure battery voltage, 3.3 V rail (IC5 out), with LDO_EN low and
   high (GSM/GPS rail), current draw. Check the battery connector holds.
4. MCU + UART: already verified (bring-up firmware). Optional: flash_selftest.
5. GSM: flash `modem_probe.bin`, key `m` (AT pass-through): `AT`, `ATI`,
   `AT+CPIN?`, `AT+CSQ`, `AT+CREG?`/`AT+CEREG?`, `AT+CGSN` (IMEI). Then `n`.
   If no answer to `AT`: check LDO rail, PWRKEY pulse (PB13 via NPN),
   UART direction (PA2/PA3), level shifting, modem supply current.
6. GPS: write a small pass-through test (USART3 9600, print NMEA), check J1
   cable, power, antenna.
7. Battery ADC (PB10) and charger pins (PB14/15): compare with a multimeter.
8. Then the main firmware `stepwhere_v1.2-lis2dh12.bin`, with full log.
9. Keep a `docs/BRINGUP_REV11.md` log: each test, result, conclusion.

## 7. Files to give the new chat

* This file (`docs/handoff/BOARD_BRINGUP_HANDOFF.md`, branch `feature/lis2dh12-steps`).
* BOM "BOM-GSM Neoway" as CSV or pasted text (not .xlsx).
* Assembly drawings R11 (top + bottom PDF). Rev11 Gerbers if available
  (only Rev10 is in the repo). A schematic does not exist.
* Full debug log of the old firmware on the new board (from power-up).
* Photos of both board sides (sharp, close-up of IC4, IC6, connectors).
* Differences between the old boards and Rev11 (parts changed, layout changes).
* Multimeter readings once available (battery, 3.3 V, GSM rail).
