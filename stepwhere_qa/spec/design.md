# Stepwhere QA Firmware — Design (v2)

Status: DRAFT v2, awaiting approval. Implements `requirements.md` v2.

## 1. Files

```
stepwhere_qa/
├── stepwhere_qa.ino        the sketch (only firmware file)
├── README.md               build, flash, report, protocol, failure codes
├── tests/
│   ├── qa_proto_test.c     host test: CRC-16 check value, JSON escaping (A2)
│   └── run.sh
└── spec/                   requirements.md, design.md, tasks.md
```

Sketch sections in order: header comment → includes → config `#define`s → pins
and objects → result model → protocol output (CRC, JSON writer) → utilities (UID,
MCU info, GPS pump) → power → accelerometer → flash → ADC → GPS → AT helper and
GSM → report → interactive mode → `setup()` / `loop()`.

## 2. Configuration (`#define`s)

| Name | Default | Use |
|---|---|---|
| `QA_FW_NAME` / `QA_FW_VERSION` | `"stepwhere_qa"` / `"QA-2.0"` | header, `QA_HELLO`, JSON |
| `QA_PROTO_VERSION` / `QA_JSON_SCHEMA` | 1 / 1 | machine lines |
| `QA_REPEAT_S` | 60 | repeat the last result (0 = off) |
| `QA_TAP_TEST` | 0 | optional tap test |
| `ACC_MAG_NOMINAL_MG` / `ACC_MAG_TOL_MG` | 1000 / 300 | ACC_XYZ |
| `ACC_ST_MIN_MG` / `ACC_ST_MAX_MG` | 68 / 1440 | ACC_SELFTEST (17 / 360 LSb × 4 mg) |
| `ACC_INT_THS_MG` | 500 | ACC_INT1/2 threshold (below 1 g) |
| `ACC_TAP_TIMEOUT_S` | 15 | ACC_TAP |
| `FLASH_JEDEC_EXPECTED` | 0xEF4018 | FLASH_ID |
| `BAT_DIVIDER` | 2 | BAT_ADC (as production) |
| `BAT_MIN_MV` / `BAT_MAX_MV` / `ADC_SATURATED_RAW` | 3000 / 4400 / 4090 | BAT_ADC |
| `GPS_UART_TIMEOUT_S` / `GPS_FIX_TIMEOUT_S` / `GPS_MIN_SATS` | 5 / 180 / 4 | GPS |
| `GSM_AT_RETRIES`, `AT_TIMEOUT_MS`, `AT_LONG_TIMEOUT_MS` | 10, 3000, 15000 | GSM |
| `SIM_READY_TIMEOUT_MS` | 10000 | GSM_SIM |
| `QA_CSQ_MIN` / `QA_CSQ_MAX` | 10 / 31 | GSM_CSQ |
| `GSM_REG_TIMEOUT_S` / `GSM_IP_TIMEOUT_S` | 60 / 30 | GSM_REG / GSM_DATA |
| `QA_APN` | `"internet"` | GSM_DATA |
| `PASSTHRU_IDLE_TIMEOUT_S` | 600 | interactive mode |

## 3. Result model

```cpp
enum TestId { T_ACC_I2C, T_ACC_WHOAMI, T_ACC_XYZ, T_ACC_SELFTEST, T_ACC_INT1, T_ACC_INT2, T_ACC_TAP,
              T_FLASH_ID, T_BAT_ADC, T_GPS_UART, T_GPS_FIX,
              T_GSM_AT, T_GSM_ATE0, T_GSM_FW, T_GSM_IMEI, T_GSM_SIM, T_GSM_ICCID,
              T_GSM_CSQ, T_GSM_REG, T_GSM_DATA, T_COUNT };
enum Outcome : uint8_t { R_NOTRUN, R_PASS, R_FAIL, R_SKIP };
struct TestResult { bool pass; char detail[48]; };
struct TestSlot {
  const char *code; const char *name; bool required;
  Outcome r; char detail[48];
  int32_t v[4];            // test-specific numbers for the JSON (meaning per test, §7.3)
  uint8_t nv;
};
```
- Each test function returns a `TestResult` and may fill `v[]`. A small runner
  stores it in `g_tests[id]`.
- `required` is fixed in the table. GPS_FIX is `false`. ACC_TAP is `QA_TAP_TEST != 0`.
  A test that never ran in this run (ACC_TAP when off) is left out of the JSON.
- No `%f` anywhere (newlib-nano has no float printf). Decimals are fixed-point
  integers (HDOP × 100, lat/lon × 10⁶).

## 4. Accelerometer (LIS2DH12, register code written directly in the sketch)

Register addresses and bit values come from `libraries/StepwhereMotion/src/lis2dh12.h`
(feature/lis2dh12-steps) and the ST datasheet. Multi-byte reads set the
sub-address MSB (0x80), which is the LIS2DH12 auto-increment rule. Each
accelerometer test starts with a BOOT (CTRL_REG5 = 0x80, wait 10 ms) and a
known state: CTRL_REG1..6, INT1/2_CFG and CLICK_CFG = 0, FIFO bypass.

### 4.1 ACC_I2C
`PA1 = HIGH` → probe 0x19 (ACK). `PA1 = LOW`, wait 1 ms → probe 0x18 (ACK) and
0x19 (no ACK). `PA1 = HIGH`. Details: `ok 0x19/0x18`, `no ACK @0x19`,
`SA0 stuck (0x19 with PA1 LOW)`, `no ACK @0x18`. With no ACK at all, a bus scan
is printed (information only).

### 4.2 ACC_XYZ
CTRL_REG4 = 0x88 (BDU, HR, ±2 g → 1 mg/digit after >> 4). CTRL_REG1 = 0x57
(100 Hz, XYZ on). Discard the first 3 samples, then read 10 when STATUS_REG
ZYXDA (bit 3) is set, 50 ms timeout each. |a| = isqrt(x² + y² + z²).
JSON: `x, y, z, mag` (means, mg).

### 4.3 ACC_SELFTEST (ST procedure from STMems_Standard_C_drivers, lis2dh12_self_test.c)
BDU on, ±2 g, normal mode (10-bit, 4 mg/digit), 50 Hz, wait 90 ms, discard 1,
average 5 → `N`. CTRL_REG4 ST = 01 (positive), wait 90 ms, discard 1, average 5
→ `S`. Per axis |S − N| must be within 68–1440 mg. Self-test off, power down.
JSON: `dx, dy, dz` (mg).

### 4.4 ACC_INT1 / ACC_INT2 (automatic, no motion)
The INT pins are push-pull and have no board pull-ups. The MCU's weak pull-ups
and pull-downs (~40 kΩ) are used as probes: a connected pin follows the sensor,
an open pin follows the MCU pull.

Sensor set-up: 100 Hz, ±2 g, no high-pass filter (gravity counts), polarity
active-high (CTRL_REG6 INT_POLARITY = 0), latched (CTRL_REG5 LIR_INT1 / LIR_INT2).
Event: OR of X-high / Y-high / Z-high (INTx_CFG = 0x2A) above 500 mg
(INTx_THS = 31 at 16 mg/LSb). With 1 g in any orientation at least one axis is
above 577 mg, so the event fires straight away.

Steps per pin:
1. Routing off (CTRL_REG3 I1_IA1 / CTRL_REG6 I2_IA2 = 0). MCU `INPUT_PULLUP`, wait 2 ms → expect **LOW** (the sensor drives the idle level).
2. MCU `INPUT_PULLDOWN`, attach a `RISING` ISR (counter only), enable routing and INTx_CFG → expect **HIGH** within 100 ms and ≥ 1 edge.
3. INTx_CFG = 0, then read INTx_SRC (clears the latch), routing off → expect **LOW** again (MCU still on pull-down).
4. Detach the ISR, MCU `INPUT`.

Details: `lo/hi/lo ok, 1 edge`, `stuck HIGH`, `never HIGH (open or not routed)`,
`stays HIGH (latch not cleared)`, `no EXTI edge`. JSON: `lo1, hi, lo2, edges`.

### 4.5 ACC_TAP (only if `QA_TAP_TEST`)
CLICK_CFG XS/YS/ZS, CLICK_THS, TIME_LIMIT; CTRL_REG3 I1_CLICK. `QA_PROMPT,ACC_TAP,Tap the board within 15 s`.
Counts INT1 rising edges for 15 s.

## 5. Flash and ADC

### 5.1 FLASH_ID
`SPIClass spi(PA7, PA6, PA5)`, 4 MHz, mode 0. /WP (PC7) and /HOLD (PB12) are driven
HIGH, /CS PC6 HIGH. Then 0xAB (release power-down), wait 50 µs, then 0x9F → 3 bytes.
Detail `EF 40 18`, or the bytes read plus a hint (`FF FF FF: no answer/MISO open`,
`00 00 00: MISO stuck low`). Read-only. JSON: `id` (string `"EF4018"`).

### 5.2 BAT_ADC
`analogReadResolution(12)`. 16 × `analogRead(AVREF)` and 16 × `analogRead(PB10)`,
averaged. VDDA = 3000 mV × VREFINT_CAL / vref_raw, where VREFINT_CAL is the factory
value at 0x1FFF75AA, measured at 3.0 V. Vbat = raw × VDDA / 4095 × `BAT_DIVIDER`.
FAIL details: `saturated (raw 4095)`, `low 2.85 V`, `high 4.61 V`.
JSON: `mv, raw, vdda_mv`.

The production firmware assumes a nominal 1212 mV for VREFINT. QA uses the factory
calibration word instead, because it is more accurate and this is a measurement test.

## 6. GPS and GSM

### 6.1 GPS
- `gps_pump()` decodes SerialGPS. It is called in every wait loop, including the AT
  helper, so the GPS keeps acquiring during the GSM tests.
- GPS_UART: counter deltas from the start of the test (TinyGPSPlus counters cannot be reset).
- GPS_FIX: TTFF is measured from LDO on (R0.1). After the GSM tests, wait until
  `GPS_FIX_TIMEOUT_S` after LDO on (or less if already fixed). Every 5 s print sats
  used, in view (GSV field 3 for GP/GL/GA/BD/GB via `TinyGPSCustom`) and HDOP.
  Fix = `location.isValid() && location.age() < 2000 && satellites ≥ 4`.
  JSON: `sats, view, hdop_x100, ttff_s` (and `lat_e6, lon_e6` if fixed).

### 6.2 AT helper
Same as spec v1 §6.2: pre-drain leftovers (logged as `[AT][URC]`), send, read
byte by byte into a static 512-byte buffer and a 128-byte line assembler, return on
a line containing `expect` (or a `>` byte if expect is ">"), fail on a line equal to
`ERROR` or starting with `+CME ERROR` / `+CMS ERROR`, timeout = fail. One `String`
assignment per call at most. Calls `gps_pump()` in its loop.

### 6.3 GSM steps
As requirements R5.2. Power-on: `AT` (3 × 300 ms) first; if no answer, PWRKEY
HIGH 100 ms → LOW 1200 ms → HIGH, wait 3 s (production timing). `[NET]` block on
GSM_REG / GSM_DATA failure: same decoding and verdicts as `net_diag()` in
`stepwhere/stepwhere.ino` v1.2-lis2dh12.2 (re-written with char buffers).
JSON: GSM_FW `fw` string; GSM_IMEI `imei`; GSM_ICCID `iccid`; GSM_CSQ `csq, dbm`;
GSM_REG `cereg, cgreg, creg`; GSM_DATA `ip` string.

### 6.4 Order and timing

| Stage | Typical | Worst |
|---|---|---|
| Header, `QA_HELLO` | < 1 s | |
| ACC ×6, FLASH_ID, BAT_ADC | ~2 s | ~3 s |
| GPS_UART | ≤ 1 s | 5 s |
| GSM 1–9 (GPS acquiring meanwhile) | 20–40 s | ~170 s |
| GPS_FIX remainder (180 s from LDO on) | 0–140 s | ~140 s |
| Total | 1–3 min | ~5.5 min |

The agent's timeout for a missing `QA_JSON` should be ≥ 8 min.

## 7. Machine protocol

### 7.1 Writer
`qa_out(const char *s)` prints to SerialDBG and, while a JSON line is open,
feeds every byte into the running CRC. Helpers: `json_key`, `json_str` (escapes
`"`, `\`, bytes < 0x20 as `\u00XX`, and bytes ≥ 0x7F as `?`), `json_int`. The
line is never built in RAM.
```
crc16_ccitt_false(crc, byte): crc ^= byte << 8; 8× { crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1 }
```

### 7.2 Line examples
```
QA_HELLO,1,stepwhere_qa,QA-2.0,0051006C 34345102 31303739
QA_JSON,1,{"schema":1,"fw":"stepwhere_qa","ver":"QA-2.0","build":"Nov  3 2026 12:00:00","core":"2.12.0","run":1,"uid":"0051006C 34345102 31303739","mcu":{"dev":"0x460","rev":"0x2000","flash_kb":128},"imei":"869938082392680","iccid":"89910000000000000000","modem_fw":"N706B-...","overall":"fail","failed":["GSM_CSQ"],"tests":[{"id":"ACC_I2C","req":1,"r":"pass","d":"ok 0x19/0x18"},{"id":"ACC_XYZ","req":1,"r":"pass","d":"|a| 1004 mg","x":12,"y":-8,"z":1003,"mag":1004},...,{"id":"GPS_FIX","req":0,"r":"fail","d":"NO FIX (3 in view)","sats":0,"view":3,"hdop_x100":0,"ttff_s":-1},{"id":"GSM_CSQ","req":1,"r":"fail","d":"CSQ 8 = -97 dBm (min 10)","csq":8,"dbm":-97}],"ms":154000}*4F1A
QA_RESULT,0051006C 34345102 31303739,869938082392680,fail,GSM_CSQ
```
`overall` and `failed` follow R6.2 (required tests only). `ms` = run duration.

### 7.3 Per-test JSON fields (besides `id`, `req`, `r`, `d`)

| Test | Fields |
|---|---|
| ACC_I2C | `addr` (25 = 0x19 found) |
| ACC_WHOAMI | `who` |
| ACC_XYZ | `x`, `y`, `z`, `mag` (mg) |
| ACC_SELFTEST | `dx`, `dy`, `dz` (mg) |
| ACC_INT1 / ACC_INT2 | `lo1`, `hi`, `lo2` (0/1), `edges` |
| ACC_TAP | `taps` |
| FLASH_ID | `id` (hex string) |
| BAT_ADC | `mv`, `raw`, `vdda_mv` |
| GPS_UART | `chars`, `ok`, `bad` |
| GPS_FIX | `sats`, `view`, `hdop_x100`, `ttff_s`, optional `lat_e6`, `lon_e6` |
| GSM_FW | `fw` |
| GSM_IMEI | `imei` |
| GSM_ICCID | `iccid` |
| GSM_CSQ | `csq`, `dbm` |
| GSM_REG | `cereg`, `cgreg`, `creg` (−1 = no answer) |
| GSM_DATA | `ip` |

### 7.4 What the agent must do (for Shubham)
1. Open the TTL port at 115200 8N1. Read lines ending in `\r\n`, up to 2048 characters. Ignore everything that does not start with `QA_`.
2. `QA_HELLO`: compare the UID with the one read over SWD. Mismatch → abort.
3. `QA_PROMPT`: show the text if there is a screen. Do not wait for it.
4. `QA_JSON`: split at the first two commas and the last `*`. Check that the protocol is 1, then the CRC (CRC-16/CCITT-FALSE, check value "123456789" → 29B1), then `JSON.parse`, then `schema`.
5. Add time, station, operator, agent version and the hash of the flashed image, then upload.
6. De-duplicate by `uid` + `run` + CRC (the result repeats every 60 s).
7. If no valid `QA_JSON` arrives within 8 minutes, report "incomplete" with the last line received.
8. `QA_RESULT` can be used as a cross-check or by simple jigs.

## 8. Interactive mode
SerialDBG RX has an MCU pull-up. Bytes are collected into a 16-byte line. Enter
runs the command: `a m g n r p s h`. Unknown → help. `p`: GSM pass-through,
`~~~` leaves (same as `modem_probe`). `s`: NMEA pass-through, Enter leaves. Both
leave after 600 s without traffic. While idle, `loop()` pumps GPS and repeats the
last result every `QA_REPEAT_S`.

## 9. Reuse
| From | What |
|---|---|
| `stepwhere/stepwhere.ino` v1.2-lis2dh12.2 | pins, UART set-up, PWRKEY timing, "AT before PWRKEY", CFUN/CPIN handling, `net_diag()` decoding, 1:2 battery divider |
| `libraries/StepwhereMotion/src/lis2dh12.h` / `.c` | LIS2DH12 register map, BOOT sequence, auto-increment bit, mg conversion |
| `libraries/StepwhereOTA/src/W25QArduino.h` | SPI pins, /WP and /HOLD high, 0xAB then 0x9F |
| `tests/lis2dh12_bringup` | SA0 0x19/0x18 check idea, RX pull-up (S-12) |
| ST `lis2dh12_self_test.c` | self-test procedure and limits |

Not reused: the production `gsm_send_at()` (flushes RX, matches "ERROR" anywhere,
grows a String byte by byte), the LIS2DS12 code from spec v1, the HTTP test.

## 10. Open points
1. **SPI library** (C2): needed for FLASH_ID. It ships with the core, like Wire.
2. **`QA_REPEAT_S` = 60**: repeating the result each minute is my addition so a late
   agent still gets it. Set it to 0 to print once.
3. ICCID command (`AT+CCID` / `AT+ICCID`) and the `AT+CGMR` reply format of the N706B
   are still unconfirmed. The parsers accept both forms.
4. The 1:2 battery divider is taken from the production code. Not verified on the
   Rev11 schematic (none exists). T9 saw a saturated ADC on one board.
