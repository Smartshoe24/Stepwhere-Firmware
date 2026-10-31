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
| T5 | 2026-10-31 | Multimeter on IC1 (TXB0102) pins, modem off / on, with `modem_power_test.bin` v0.2 | FAIL: middle pins read 0 V (partly a probing miss, see T6) |
| T6 | 2026-10-31 | Re-check IC1 middle pins at the pads (modem on) | **FAIL: VCCB (pin 7) = 0 V.** VCCA 1.8 V, OE 1.8 V, GND 0 V are correct. Root cause of "no AT answer" |
| T7 | 2026-10-31 | Battery off: continuity checks around IC1 pin 7, then give VCCB 3.3 V, re-run `t` | Part 1 done: empty footprint above IC1 feeds pin 7; its other pad is not on 3V3. Part 2 OPEN |

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

### T5 first reading (2026-10-31)

Owner marked an 8-pin chip on the **top side**, right of the N706B, between
the SOT-23 transistors, and wrote on the photo: **left side 3.31 V, right
side 3.19 V**. Not yet known: which pin(s) were measured, in which test state
(`o` / `b` / `0`), and whether this chip is IC1 (TXB0102) (marking not read
yet).

Notes:

* One reading per side is not enough: every TXB0102 has a GND pin (0 V), so
  not all four pins on one side can be at 3.2-3.3 V. Asked for all 8 pins.
* If one supply pin is 3.31 V (VCCB, MCU side) and the other is 3.19 V
  (VCCA, modem side), the modem side of the level shifter runs at ~3.2 V.
  Neoway LTE modules usually have 1.8 V UART pins (to be checked in the N706B
  hardware guide / the owner's design). A 3.2 V VCCA would mean the modem's
  1.8 V "high" is not seen as high, and the modem RXD input gets 3.2 V.
  **Idea only, not confirmed.**
* It also does not fit T4 well: PA3 was floating with PA10 on before the
  power-key pulse. With both supplies at ~3.3 V all the time, the TXB0102
  would drive PA3 even then. So the measurement state matters.

Asked for: per-pin readings in state `o` and `b`, chip marking, close-up photo.

15:47 (screenshot): the reset over SWD failed with "No debug probe
detected" (ST-LINK not connected to the Mac). The board was running anyway,
and the automatic test `t` ran a second time with the same result as T4: PA3
floating after PA10 on, driven HIGH after the method A pulse, no answer at
any baud rate. **T4 is repeatable.**

## T5 result: IC1 pin voltages (2026-10-31)

8-pin chip on the top side, right of the N706B (close-up photo from the
owner). Measured against GND, `o` = modem off, `b` = modem on (v0.2):

| Pin | o (off) | b (on) |
|---|---|---|
| 1 | 3.19 V | 3.3 V |
| 2 | 0 | 0 |
| 3 | 0 | 0 |
| 4 | 0.28 V | 1.8 V |
| 5 | 0.28 V | 1.8 V |
| 6 | 0 | 0 |
| 7 | 0 | 0 |
| 8 | 3.3 V | 3.3 V |

TXB0102 in VSSOP-8 (DCU) has its four data pins on the corners (B2 and B1
facing each other at one end, A2 and A1 facing each other at the other end)
and GND, VCCA, OE, VCCB on the four middle pins. This holds whichever corner
was counted as pin 1, so the conclusion does not depend on the numbering:

* Pins 1 + 8 = **B side (MCU, 3.3 V)**: PA2 (MCU TX, idles HIGH) and PA3
  (MCU RX, held up by the STM32 internal pull-up: 3.19 V with modem off).
  As expected.
* Pins 4 + 5 = **A side (modem)**: 1.8 V with the modem on, ~0.28 V off.
  The modem's UART lines idle HIGH at 1.8 V, so **the modem is on and its
  UART is at 1.8 V**. This also confirms a level shifter is needed.
* Pins 2, 3, 6, 7 = **GND, VCCA, OE, VCCB: all 0 V, modem off and on.** One
  of them should be 0 V (GND). The other three should be VCCB = 3.3 V,
  VCCA = 1.8 V (modem on) and OE = high.

Result: **FAIL. The level shifter has no supply and is not enabled, so no
data can pass in either direction.** This explains T2 and T4 exactly: the
modem starts, but never receives `AT` and its answers never reach the MCU.
(TXB0102 data pins go high-impedance when unpowered, which is why pins 1/8
and 4/5 still show the levels of the MCU and the modem.)

Caveat: the middle pins are hard to probe at 0.5 mm pitch; four readings of
exactly 0 V could also be a probe that did not touch. Re-check at the pads
(T6).

Close-up photo: just **above IC1, slightly left of centre, there seem to be
two small round pads with no part between them** (an empty 0402 footprint?).
The BOM lists three 0 R parts (R26, R27, R33). If that footprint is the
0 R / link that feeds IC1's supply, it would explain the 0 V. Not confirmed:
needs the design (owner) or a close look.

## T6: Re-check and find the missing supply

Status: OPEN. Asked the owner to:

1. Re-measure pins 2, 3, 6, 7 with the modem on (`b`), probe on the solder
   joint at the pad, not on the leg.
2. Look at the spot above IC1 with a magnifier: empty footprint or not?
   Measure both pads of it with the modem on.
3. Check in the DesignSpark design which nets feed IC1 VCCA, VCCB and OE,
   and which part is at that footprint. Compare with an old working board.
4. **Do not bridge anything yet**: VCCA must get the modem's 1.8 V (VDD_EXT),
   not 3.3 V, or the modem's UART pins may be damaged.

## T6 result: IC1 middle pins re-measured (2026-10-31)

Owner confirmed the part marking/type: **TXB0102DCUR**. Modem on (`b`),
probe on the pads:

| Pin | TXB0102 DCU function (TI pinout) | Measured | Expected | |
|---|---|---|---|---|
| 1 | B2 (MCU side data) | 3.3 V (T5) | 3.3 V idle | OK |
| 2 | GND | 0 V | 0 V | OK |
| 3 | VCCA (modem side supply) | 1.8 V | 1.8 V (modem VDD_EXT) | OK |
| 4 | A2 (modem side data) | 1.8 V (T5) | 1.8 V idle | OK |
| 5 | A1 (modem side data) | 1.8 V (T5) | 1.8 V idle | OK |
| 6 | OE (enable) | 1.8 V | = VCCA | OK |
| 7 | **VCCB (MCU side supply)** | **0 V** | **3.3 V** | **FAIL** |
| 8 | B1 (MCU side data) | 3.3 V (T5) | 3.3 V idle | OK |

The owner's pin numbering matches the TI pinout: the data pins, VCCA and OE
all read the voltages they should.

Result: **ROOT CAUSE FOUND (to be confirmed by the fix). VCCB of the level
shifter IC1 has no 3.3 V.** The TXB0102 needs VCCA <= VCCB; with VCCB = 0 V
it does not translate in either direction. So the modem starts (VDD_EXT 1.8 V
powers VCCA and OE, which is why PA3 changed after the power-key pulse in
T4), but `AT` never reaches it and nothing comes back.

The 0 V in T5 on pins 3 and 6 was a probing miss; pins 2 and 7 were right.

Open: why pin 7 has no 3.3 V. Candidates: a part not fitted (the empty
footprint above IC1 in the close-up, maybe one of the 0 R parts R26 / R27 /
R33), an open trace or via, a bad solder joint on pin 7, or pin 7 tied to GND
by mistake in the Rev11 layout.

## T7: Find the break, then give VCCB 3.3 V

Status: OPEN, steps sent to the owner. Plan:

1. Battery off (unsolder the red wire at BAT), so resistance / beep tests
   are safe.
2. Beep test: IC1 pin 7 to GND (must NOT beep; if it beeps, pin 7 is shorted
   to GND: do not add 3.3 V). IC1 pin 7 to the 3V3 pin of the SWD header
   (should beep; it will not, if the link is broken). Both pads of the empty
   footprint above IC1 to pin 7 and to 3V3.
3. Fix: fit the missing 0 R / solder bridge if the footprint is the link,
   otherwise a thin wire from IC1 pin 7 to 3V3 (header pin or a nearby 3.3 V
   pad). 3.3 V only on pin 7, never on pins 3 or 6 (1.8 V side).
4. Battery back, flash `modem_power_test.bin`, press `t`. Expected:
   `AT OK at 115200`, plus ATI / CPIN / CSQ / IMEI answers.
5. Then the owner's LIS2DS12 firmware again (GSM + GPRS should come up).

### T7 part 1: beep tests, battery off (2026-10-31)

| From | To | Beep? | Meaning |
|---|---|---|---|
| IC1 pin 7 (VCCB) | GND | no | Good: pin 7 is not shorted, 3.3 V can be added safely |
| IC1 pin 7 | 3V3 pin of the SWD header (MCU 3.3 V) | no | The 3.3 V connection to VCCB is broken / missing |
| Empty footprint above IC1, pad "P7" | IC1 pin 7 | yes | This footprint is in the VCCB supply path |
| Empty footprint, other pad "PX" | 3V3 header pin | no | PX is not directly on the MCU 3.3 V rail |

Result: the empty footprint (part not fitted) feeds VCCB of IC1. Its other
pad PX is not on the 3V3 header net, so PX may be on another rail (for example
"3.3VL" through the unknown series part, or a switched rail) or on nothing.
A beep tester also stays silent through a resistor of more than a few tens
of ohms, so PX could still reach 3.3 V through a part.

Next (T7 part 2): battery back on, modem on (`b`), measure the voltage on PX
and P7. If PX = 3.2-3.3 V: bridge the footprint (0 R or solder bridge). If
not: thin wire from P7 to the 3V3 header pin instead. VCCB must be the MCU's
3.3 V in any case, because the B side talks to PA2/PA3.

Pad positions (owner, beep test, battery off): with the modem on the left as
in the close-up photo, **P7 = left pad** of the empty footprint above IC1
(beeps to IC1 pin 7), **PX = right pad**.
