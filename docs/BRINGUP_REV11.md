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
| T2 | 2026-10-31 | Full boot log of v1.2 firmware on Rev11, from power-up | OPEN |

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
