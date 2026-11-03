# Stepwhere QA Firmware — Requirements (v2)

Status: DRAFT v2, awaiting approval (Diksha). No QA code is written before approval.
Branch: `feature/qa-firmware` (from `feature/lis2dh12-steps`), its own draft PR.
Replaces v1 of this spec, which targeted a G071 and a LIS2DS12.

## 1. Purpose and scope

A factory test firmware for freshly assembled Stepwhere GSM boards (Rev11 and
later). On every boot it tests each hardware block once, without any input,
and prints:

1. a human-readable log and report table, and
2. machine lines (`QA_HELLO`, `QA_PROMPT`, `QA_JSON`, `QA_RESULT`) that the
   existing **QA bench agent** (v0.1.0, Node, on the bench PC) reads from the
   debug TTL. The agent flashes the board over SWD, matches the unit by its
   STM32 UID and uploads the result to panel.stepwhere.in.

Out of scope: any uploader in the firmware, sending QA results over GSM, HTTP
or HTTPS, sleep, step counting, battery management, OTA.

## 2. Constraints

- C1. Work only in `stepwhere_qa/` on `feature/qa-firmware`. Production code is
  read-only reference.
- C2. One sketch file, `stepwhere_qa/stepwhere_qa.ino`. Libraries: TinyGPSPlus 1.0.3,
  Wire, HardwareSerial and **SPI** (core library, needed for FLASH_ID; see Q1).
  The repo's `libraries/` are not used, so the sketch builds on its own.
- C3. Target **STM32G070CBT6**, FQBN `STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX`,
  STM32duino core **2.12.0**. Builds with 0 warnings from `stepwhere_qa.ino` at
  warning level "All". Warnings inside the core or TinyGPSPlus are reported, not fixed.
- C4. **No input needed.** The full test runs on boot. Nothing waits for the agent,
  for an operator or for board RX. The tap test is off by default.
- C5. Every wait has a timeout. ISRs only count (`volatile`), no printing.
- C6. Timeouts, thresholds, APN and enable flags are `#define`s at the top.
- C7. Each test is its own function returning `TestResult { bool pass; char detail[48]; }`
  plus test-specific numbers for the JSON.
- C8. Bounded buffers. The `QA_JSON` line is printed piece by piece, not built in RAM.

## 3. Hardware (Rev11, from the production firmware and the bring-up log)

| Block | Interface / pins | Notes |
|---|---|---|
| MCU | STM32G070CBT6, 128 KB flash, 36 KB RAM | UID at 0x1FFF7590 |
| Debug / agent TTL | USART1 TX PB6, RX PB7, 115200 8N1 | MCU pull-up on RX (an unconnected RX picks up noise) |
| GSM Neoway N706B | USART2 TX PA2, RX PA3, 115200; PWRKEY PB13 (idle HIGH, LOW ≥ 1.2 s = on) | |
| GSM/GPS LDO | LDO_EN PA10 (HIGH = on) | |
| GPS G7A | USART3 TX PB8, RX PB9, 9600; J1 | passive chip antenna (Abracon ACAG0301-1575-T) |
| Accelerometer LIS2DH12 (IC7) | I2C SDA PA12, SCL PA11; SA0 = PA1 (HIGH → 0x19, LOW → 0x18); INT1 PB3, INT2 PB4 | no pull-ups on INT1/INT2 (push-pull outputs) |
| External flash W25Q128 (IC8) | SPI1 MOSI PA7, MISO PA6, SCK PA5; /CS PC6; /WP PC7, /HOLD PB12 driven HIGH | JEDEC ID EF 40 18 |
| Battery sense | PB10 (ADC), 1:2 divider (as the production firmware assumes) | VDDA measured with VREFINT |

## 4. Functional requirements

### R0. Start
- R0.1 Drive PWRKEY HIGH, then LDO_EN HIGH (GSM and GPS get power; the GPS
  time-to-first-fix starts here).
- R0.2 Print a header with firmware name, version, build date/time, core version,
  MCU device ID / revision / flash size, and the UID.
- R0.3 Print `QA_HELLO` (R7.2) before any test, so the agent can match the unit early.

### R1. Accelerometer (LIS2DH12)
| Code | Required | Test | PASS when |
|---|---|---|---|
| ACC_I2C | yes | PA1 HIGH: ACK at 0x19. PA1 LOW: ACK at 0x18 and none at 0x19. PA1 back HIGH | all three as expected (proves the bus and the SA0 line). On no ACK at all: print a bus scan 0x08–0x77 |
| ACC_WHOAMI | yes | read 0x0F | 0x33 |
| ACC_XYZ | yes | 100 Hz, high-res 12-bit, ±2 g, BDU; 10 samples after DRDY | no sample all-zero and mean \|a\| = 1000 ± 300 mg (orientation does not matter) |
| ACC_SELFTEST | yes | ST procedure (ST driver example): ±2 g, normal 10-bit, 50 Hz, average 5 samples with self-test off and with positive self-test | change on each axis 68–1440 mg (17–360 LSb) |
| ACC_INT1 | yes | pin driven both ways by the sensor, see design §4.4 | LOW with MCU pull-up, HIGH with MCU pull-down, LOW again, and ≥ 1 EXTI edge |
| ACC_INT2 | yes | same for INT2 | same |
| ACC_TAP | only if `QA_TAP_TEST` = 1 (default 0) | `QA_PROMPT`, single tap on INT1 within 15 s | ≥ 1 tap. Not run and not counted when disabled |

If ACC_I2C fails, the other ACC tests are `skip` (and count as failed).

### R2. External flash (W25Q128)
| FLASH_ID | yes | release from power-down (0xAB), read JEDEC ID (0x9F) | EF 40 18. Detail shows the bytes read (FF FF FF = no answer / MISO open, 00 00 00 = MISO shorted) |
Read-only: nothing is erased or written.

### R3. Battery ADC
| BAT_ADC | yes | 16 readings of PB10 and VREFINT; VDDA from the factory VREFINT calibration; Vbat = Vpin × 2 | not saturated (raw < 4090) and 3.0 V ≤ Vbat ≤ 4.4 V. Value always printed (mV, raw, VDDA) |

### R4. GPS (G7A)
| Code | Required | Test | PASS when |
|---|---|---|---|
| GPS_UART | yes | up to 5 s | TinyGPSPlus `passedChecksum()` grew (any talker) |
| GPS_FIX | **no (info)** | runs in the background during the GSM tests; final wait until `GPS_FIX_TIMEOUT_S` (180) after LDO on | valid location with ≥ 4 satellites. Reports satellites used / in view (GSV), HDOP, TTFF. Never affects OVERALL |

If GPS_UART fails, GPS_FIX is `skip`.

### R5. GSM (Neoway N706B)
- R5.1 Power-on: send `AT` first and pulse PWRKEY only if there is no answer (as production v1.2-lis2dh12.2).
- R5.2 One AT helper (as in spec v1): no RX flush during a response, leftovers logged as URC,
  returns on `expect` or on an `ERROR` / `+CME ERROR` / `+CMS ERROR` line, logs command and response.

| # | Code | Required | Command | PASS when |
|---|---|---|---|---|
| 1 | GSM_AT | yes | `AT`, ≤ 10 tries 1 s apart | OK |
| 2 | GSM_ATE0 | yes | `ATE0`, then `AT+CMEE=2` (not graded) | OK |
| 3 | GSM_FW | yes | `AT+CGMR` | OK; version string reported |
| 4 | GSM_IMEI | yes | `AT+CGSN` | exactly 15 digits |
| 5 | GSM_SIM | yes | `AT+CFUN?` (→ `AT+CFUN=1` if not 1), poll `AT+CPIN?` ≤ 10 s | `READY`; otherwise "SIM missing/locked (<CPIN/CME text>)" |
| 6 | GSM_ICCID | yes | `AT+CCID`, on ERROR `AT+ICCID` | 19–20 characters [0-9A-F] |
| 7 | GSM_CSQ | yes | `AT+CSQ` | `QA_CSQ_MIN` (10) ≤ rssi ≤ 31; rssi and dBm reported. 99 = FAIL. The wire is the real antenna, so a weak signal is a defect |
| 8 | GSM_REG | yes | poll `AT+CEREG?`, `AT+CGREG?`, `AT+CREG?` ≤ 60 s | any stat 1 (home) or 5 (roaming) |
| 9 | GSM_DATA | yes | `AT+CGDCONT=1,"IP",QA_APN`, `AT+XIIC?` (already up?), `AT+XIIC=1`, poll `AT+XIIC?` ≤ 30 s | state 1 and an IP that is not 0.0.0.0 |

- R5.3 Skip rules: GSM_AT fails → 2–9 `skip`. GSM_SIM fails → 6–9 `skip`. GSM_REG fails → 9 `skip`.
- R5.4 After a GSM_REG or GSM_DATA failure, print the same decoded `[NET]` block as the
  production firmware (CSQ in dBm with rating, CEREG/CGREG, COPS, modem APN, CEER, verdict).
  The verdict text also goes into the test detail.
- R5.5 `QA_APN` default `"internet"` (Airtel IoT M2M; supplier APN may be `"airteliot.com"`).
- R5.6 No HTTP test, no `AT+CFUN=0`. The modem stays on.

### R6. Report
- R6.1 Table: one row per test (code, PASS/FAIL/SKIP/INFO, detail), then OVERALL, with UID and IMEI in the header.
- R6.2 OVERALL = PASS only if every **required** test passed. `skip` counts as failed.
  GPS_FIX never counts; ACC_TAP counts only when enabled.

### R7. Machine protocol (approved 2026-11-03)
All machine lines start at the beginning of a line with `QA_`, end with `\r\n`,
and contain only printable ASCII.

- R7.1 Order per run: `QA_HELLO` → (`QA_PROMPT` only if the tap test is on) → `QA_JSON` → `QA_RESULT`.
- R7.2 `QA_HELLO,1,stepwhere_qa,<version>,<uid>`
  e.g. `QA_HELLO,1,stepwhere_qa,QA-2.0,0051006C 34345102 31303739`
- R7.3 `QA_PROMPT,<test code>,<text>` (information for an operator; nothing depends on it).
- R7.4 `QA_JSON,1,<json>*<crc>`: one line, at most 2048 characters. `<crc>` is
  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0) over
  every byte of `<json>`, as 4 upper-case hex digits. Check value: "123456789" → `29B1`.
- R7.5 `QA_RESULT,<uid>,<imei|UNKNOWN>,<pass|fail>[,<failed code>...]` (no trailing comma).
- R7.6 UID format: `%08lX %08lX %08lX` of UID words w0, w1, w2 (0x1FFF7590, +4, +8).
  To be confirmed on the first board against the UID the bench reads over SWD.
- R7.7 JSON content: see design §7 (schema 1). Strings are JSON-escaped. Every test
  appears once with `id`, `req` (1/0), `r` (`pass`/`fail`/`skip`) and its numbers.
- R7.8 Each run has a `run` number (1 after reset). After the run, the board repeats
  `QA_HELLO`, `QA_JSON` and `QA_RESULT` of the last run every `QA_REPEAT_S` (60 s,
  0 = off), so an agent that connects late still gets the result. The agent
  de-duplicates by uid + run + crc.

### R8. Optional interactive mode (for a technician with a terminal)
- Commands are a letter **followed by Enter**, so single noise bytes on an open RX
  line do nothing: `a` all, `m` accelerometer + flash + ADC, `g` GPS, `n` GSM,
  `r` reset, `p` AT pass-through (`~~~` leaves), `s` NMEA pass-through (Enter leaves), `h` help.
- A re-run (`a`/`m`/`g`/`n`) increments `run` and prints a full new set of machine lines.
- Pass-through modes also end after 600 s without traffic.

### R9. Documentation
`stepwhere_qa/README.md`: board settings, libraries, build (Arduino IDE and
arduino-cli), flashing with STM32CubeProgrammer (GUI and CLI, ST-LINK SWD, 0x08000000),
how to read the report, the protocol (R7) for the agent with a parse checklist, and a
table of every code with its likely causes.

## 5. Acceptance
- A1. Compiles as in C3 with 0 warnings from `stepwhere_qa.ino`; flash/RAM usage reported.
- A2. Host check of the CRC function (check value) and of the JSON escaping, using the
  same C code as the sketch (copied into a host test under `stepwhere_qa/tests/`).
- A3. Self-review against R0–R9 and C1–C8; `git diff` touches only `stepwhere_qa/`.
- A4. On hardware (by the team): UID matches the bench, machine lines parse, report
  as expected on a known-good board.
