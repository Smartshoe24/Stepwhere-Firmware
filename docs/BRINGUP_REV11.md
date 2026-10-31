# Rev11 GSM PCB bring-up log

Board: Stepwhere GSM PCB **Rev11** (assembly "R11 STEPWHERE 16.08.2026"),
first test after assembly. Branch for this work: `bringup/rev11` (from
`feature/lis2dh12-steps`). Background: `docs/handoff/BOARD_BRINGUP_HANDOFF.md`.

Goal: find out why the firmware that worked for months on the old boards
(v1.2, LIS2DS12) does not work on Rev11.

Result words: **PASS** = works as expected, **FAIL** = does not work,
**OPEN** = waiting for data from the owner.

## Summary

| # | Date | Test | Result |
|---|---|---|---|
| T0 | 2026-10-31 | Earlier tests with the bring-up firmware (from the handoff) | PASS for MCU, UART, accelerometer, flash; power connector is weak |
| T1 | 2026-10-31 | Symptom report from the owner | OPEN |
| T2 | 2026-10-31 | Full boot log of the owner's working LIS2DS12 firmware on Rev11, from power-up | OPEN |
| T3 | 2026-10-31 | Photos (3) and BOM review | Done, 1 open item (J1 GPS connector empty in photos) |

## T0: Already known before this log (from the handoff)

Done with `prebuilt/lis2dh12_bringup.bin` v0.5:

* PASS: MCU runs, SWD flashing, debug UART (PB6/PB7, 115200).
* PASS: LIS2DH12 at 0x19, data, FIFO, INT1. INT2: 1 pass, 1 fail (recheck).
* PASS: W25Q128 (JEDEC EF 4018), write/read.
* PASS: step counter on the bring-up firmware (192 of 202 steps).
* FAIL (fixed for testing): ~40 brown-out resets (RCC_CSR 0x0C) while the
  board was moved. Cause: weak power connector. Powered from the battery,
  board not moved: 10+ min without reset.
* Never tested: GSM modem, GPS, battery ADC, charger pins, GSM/GPS supply
  switched by PA10, LEDs, current. The bring-up firmware keeps PA10 LOW, so
  **the modem has never had power in any test so far.**

## T1: Symptom report

Owner's words (2026-10-31): on the old boards the firmware starts GSM first,
then GPS. GSM is usually connected within ~10 s and GPS gets a fix within the
next ~2 min. On Rev11 the firmware gets **stuck at starting the device**.

Not yet known: which line of the debug log it stops at, whether it resets and
starts again, which binary was flashed. -> T2.

What the code does at start-up (both v1.2 original and v1.2-lis2dh12), in order:

1. Banner `[SETUP]: === STM32G071CB — N706 GSM / G7A GPS Tracker ===`
2. Charger status, ADC / battery print
3. `[LDO] ON` (PA10 HIGH: power to GSM + GPS)
4. Accelerometer init (old firmware: `LIS2DS12 not found !!!` is expected on
   Rev11 and does not stop the start-up; the I2C writes just get no answer)
5. `[TIMER] one minute timer started`
6. `[GSM] Power ON ...`, PWRKEY pulse on PB13, `[GSM] Waiting for boot ...`,
   `[GSM] Power ON complete.`
7. Up to 5 tries of: `[Modem] Checking AT ...`, SIM (`AT+CPIN?`), IMEI, GPRS
8. `[SETUP] Init complete.` then loop() with GPS and HTTP

Working ideas (to be checked, not proven):

* A. The board resets when the modem is switched on (step 3 or 6): the modem
  draws a lot of current at once, and this board already showed brown-out
  resets from the weak power connector. Sign in the log: the banner (1)
  appears again and again.
* B. The modem gets no power or does not boot (PA10 supply, PWRKEY driver
  PB13/NPN): log shows `[Modem] ERROR: No response to AT` and retries.
* C. Modem answers but SIM / network fails (SIM, antenna): log shows CPIN,
  CSQ or GPRS errors.

## T2: Full boot log of v1.2 on Rev11

Status: OPEN. Steps sent to the owner on 2026-10-31.

Owner decided to run first **his own working firmware (LIS2DS12 version, the
one used on the old boards)**, not `stepwhere_v1.2-lis2dh12.bin`. This is the
exact case that fails, so it is the right first test. Expected on Rev11:
`LIS2DS12 not found !!!` (harmless). GSM + GPS antennas are now connected.

Because the battery is now soldered to the board, the log is started from a
reset (flash with `-rst`, or a reset over SWD) instead of from plugging in the
battery.

## T3: Photos and BOM review (2026-10-31)

Three photos (top side with N706B, bottom side twice) and the BOM
"BOM-GSM Neoway" as CSV.

Seen in the photos:

* Modem marked **N706B-CN** (LTE module).
* Battery wires are **soldered** to the BAT and GND pads. This replaces the
  weak connector that caused the brown-out resets in T0. The 5V pad (charger
  input) is not connected.
* SIM card is inserted (photo 3). Some flux / dirt is visible on the SIM
  holder pins (photo 2); not a problem for now.
* **J1 (white 4-pin FFC connector, bottom side) is empty in all photos.** The
  firmware reads GPS from a G7A module on USART3 (PB8/PB9). If the GPS module
  connects through J1, GPS cannot work in this setup. Asked the owner.
* Bottom-side 6-pin header is labelled (as far as readable) DIO, CLK, RST,
  3V3, GND: the SWD (programming) header.
* Two wires (black, brown) are soldered at the top edge near the modem.
  Purpose not known yet (debug UART?). Asked the owner.
* Photo resolution is too low to judge solder bridges on IC4 (LQFP48) or the
  N706B pads. Nothing obviously wrong.

From the BOM:

* IC3 AP22811**A**: USB-style power switch, enable input is active HIGH,
  2 A max ([Digi-Key AP22811AW5-7](https://www.digikey.com/en/products/detail/diodes-incorporated/AP22811AW5-7/6602437),
  [Diodes datasheet](https://www.mouser.com/datasheet/2/115/AP22811-2997717.pdf)).
  Input 2.7-5.5 V, 0.6 ms soft-start, fault flag. This fits "PA10 HIGH
  switches on the GSM/GPS supply". Not confirmed on the board.
* D3 PZ3D4V2H: 5.1 V zener, matches the BOM text
  ([datasheet](https://hetpro-store.com/PDFs/PZ3D4V2H.pdf)).
* Largest capacitors are 10 uF (C7, C13, C16, C21, C23). There is no large
  reservoir capacitor (around 100 uF or more) on the BOM. LTE modules draw
  short current bursts during network registration, and a small reservoir
  makes the supply dip. Idea for later, not a finding: compare with the old
  boards.
* IC7 line still describes the LIS2DS12 (1.62-1.98 V); LIS2DH12 is fitted.

Open questions to the owner: what is on J1 (GPS?), what are the black/brown
wires, which modem did the old boards use (N706 or N706B-CN), same reservoir
capacitors on the old boards?
