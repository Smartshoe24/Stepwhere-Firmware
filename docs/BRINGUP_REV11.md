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
| T1 | 2026-10-31 | Symptom report from the owner | Done: "stuck at starting" = modem never answers AT (see T2) |
| T2 | 2026-10-31 | Boot log of the owner's working LIS2DS12 firmware on Rev11 (8 min, partial copy) | FAIL: modem does not answer `AT`; no MCU resets seen; battery 3.87 V |
| T3 | 2026-10-31 | Photos (3) and BOM review | Done, 1 open item (J1 GPS connector empty in photos) |
| T4 | 2026-10-31 | `modem_power_test.bin` v0.1: modem power, power key polarity, UART line, baud/swap scan, GPS | FAIL: modem seems to start (PA3 becomes driven), but no byte at any baud, normal or swapped; GPS silent |
| T5 | 2026-10-31 | Multimeter on IC1 (TXB0102) pins, modem off / on / PA2 low, with `modem_power_test.bin` v0.2 | OPEN |

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

## T2 result: boot log with the owner's LIS2DS12 firmware (2026-10-31)

Owner ran his working firmware for ~8 min with `screen` at 115200; only the
last part could be copied (the start-up part is missing). Excerpt:

```
[GSM] Power ON ...
[LDO] ON
[GSM] Waiting for boot ...
[GSM] Power ON complete.
[STATUS] Powered ON   : NO   (IMEI, GPRS, GPS, Ready: all NO)
[ADC] vBAT  raw  : 2397
[ADC] vBAT  V   : 3.87
[ADC] Vref raw  : 1501
[ADC] VDDA V   : 3.30
[BATTERY] : OK
[MAIN] System not ready — re-running init sequence ...
[Modem] Checking AT ...
[AT][CMD] >> AT
..[AT][RSP] <<            (empty, 5 times)
[Modem] ERROR: No response to AT
[MAIN] Modem lost — power cycling ...
[GSM] Power OFF ...
[AT][CMD] >> AT+CFUN=0
....[AT][RSP] <<          (empty)
[GSM] Power ON ...        (and again, the same cycle)
```

Result: **FAIL**. The modem gives **no byte at all** on USART2, not even
garbage. "Stuck at starting the device" = this endless retry loop.

Conclusions:

* The MCU keeps running in loop() and does not restart (no start banner in
  the copied part). Idea A (board resets when the modem switches on) is
  unlikely. Not fully excluded, because the first minutes are missing.
* Battery 3.87 V and VDDA 3.30 V are fine (MCU's own ADC).
* Remaining causes (idea B): modem gets no supply after PA10, the power key
  pulse (PB13) does not switch the modem on (wrong polarity or broken NPN
  driver), or the UART path PA2/PA3 (possibly through level shifter IC1
  TXB0102) is broken or swapped. Idea C (SIM / network) is ruled out for now:
  the modem would at least answer `AT` without a SIM.
* Side note on the firmware: its "power cycling" never switches PA10 off
  (`ldo_enable(false)` is commented out in `gsm_power_off()`), and every
  retry sends another power-key pulse. A hung modem is therefore never really
  restarted. Not the cause here, but worth fixing later.
* `Steps in last minute: 0` is expected: this firmware looks for the LIS2DS12.

## T4: modem power test firmware (2026-10-31)

New test sketch `tests/modem_power_test/modem_power_test.ino`, prebuilt
`prebuilt/modem_power_test.bin` (v0.1, 0x08000000, built with STM32duino
2.12.0, Generic G070CBTx). Key `t` runs everything once and prints a summary:

1. PA10 LOW 5 s, then reads the modem->MCU line PA3 with the internal
   pull-down and pull-up: same value both times = somebody drives the line
   (modem UART alive), different = floating (no power on the modem side).
2. PA10 HIGH, read PA3 again.
3. Method A: power-key pulse like v1.2 (PB13 idle HIGH, LOW 1.2 s). Listen
   15 s for boot messages, then `AT` at 115200, then a scan of 8 baud rates
   with normal and swapped TX/RX (STM32 USART SWAP bit).
4. PA10 LOW 5 s.
5. Method B: opposite polarity (PB13 idle LOW, HIGH 1.2 s), same checks.
   This is the correct polarity if PWRKEY is driven through an NPN
   transistor (PB13 HIGH = transistor on = PWRKEY pulled low).
6. GPS: listen 5 s on USART3 (PB9 RX, 9600).

Battery voltage is sampled during steps 3 and 5 to catch supply dips.

Status: OPEN, steps sent to the owner.

## T4 result (2026-10-31, 15:25)

Full log: owner's `rev11-T4` file. Summary printed by the board:

```
modem_power_test v0.1
Battery idle / lowest A / lowest B : 3.86 / 3.85 / 3.85 V
PA3 power off      : floating (nobody drives it: modem/level shifter has no power)
PA3 power on       : floating (nobody drives it: modem/level shifter has no power)
PA3 after method A : driven HIGH (modem UART side is alive)
PA3 after method B : driven HIGH (modem UART side is alive)
Boot bytes A / B   : 0 / 0
Method A           : no answer at any baud rate, normal or swapped
Method B           : no answer at any baud rate, normal or swapped
GPS                : 0 bytes in 5 s (GPS silent)
```

Result: **FAIL**, but it narrows the problem down.

* PA3 (modem -> MCU line) is floating with PA10 on and **becomes driven HIGH
  only after the power-key pulse**. A UART line idles HIGH, so something on
  the modem side switches on after the pulse. The most likely explanation:
  IC1 (TXB0102 level shifter) gets its low-voltage supply VCCA from the
  modem's own 1.8 V output (VDD_EXT), which only exists while the modem runs.
  So **the modem very probably powers up and starts**, with both pulse
  polarities. Not proven until VCCA is measured (T5).
* Still **zero bytes** in 2 x 15 s of listening and no answer to `AT` at 8
  baud rates, normal and swapped. So the data path between MCU and modem
  does not work, or the MCU is connected to a modem UART that does not speak
  AT (debug / AUX UART instead of the main UART), or the modem does not finish
  booting.
* Battery (PB10) did not dip (3.86 -> 3.85 V). This only covers the battery
  side; whether PB10 measures before or after the power switch is UNKNOWN.
* GPS silent: matches J1 (GPS connector) being empty in the photos (T3).

Next: find out where the signal stops, with a multimeter on IC1 (T5), and ask
the owner (who designed the board in DesignSpark, see the Rev10 plot report
path) how the modem UART is routed on Rev11.

## T5: Multimeter on IC1 (TXB0102)

`modem_power_test.bin` v0.2 adds measuring states: `o` modem off, `b` modem on
and stays on, `0` / `1` hold PA2 (MCU TX) LOW / HIGH as a plain pin, `u` PA2
back to UART.

TXB0102: VCCA (1.2-3.6 V, low side) must be <= VCCB (high side). Expected if
the design is right: VCCB = 3.3 V always; VCCA = ~1.8 V only with the modem
on; OE = same as VCCA (enabled); one A/B pair follows PA2 (`0`/`1`), the
other pair idles HIGH (modem TX). The pin numbers are not assumed: the
supply pins identify themselves by their voltages.

Status: OPEN, steps sent to the owner.
