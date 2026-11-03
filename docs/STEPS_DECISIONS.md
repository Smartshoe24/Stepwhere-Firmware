# Stepwhere steps: decision log (LIS2DH12 software step counter)

Why the step counter is built the way it is. Add a row whenever a decision
is made; never delete one, mark it "Replaced by S-xx" instead. Context:
`docs/handoff/LIS2DH12_HANDOFF.md` on `ota/foundation`.

| ID | Date | Decision | Why | Alternatives rejected |
|---|---|---|---|---|
| S-01 | 2026-10-30 | Pins from the Rev10 Gerbers (traced copper, table below). Same MCU pins as the LIS2DS12 firmware, address 0x19 | No schematic exists; copper trace matched against the fixed STM32G070 LQFP48 pinout and the known W25Q128 / SWD header pins | Guess from old firmware |
| S-02 | 2026-10-30 | Own small plain-C driver (`lis2dh12.c`, 1.5 KB) with an Arduino Wire wrapper, like `w25q.c` | Only ~15 registers needed; same style as the OTA code; tested against a register-level fake | ST `lis2dh12-pid` (larger, every register, harder to fake) |
| S-03 | 2026-10-30 | Recordings at 50 Hz, 12-bit, +-8 g; counter runs on 25 Hz (pairs averaged). Planned firmware setting: 25 Hz, 10-bit, +-8 g, FIFO watermark 24 on INT1 (about 1 wake/s), motion interrupt on INT2 | Heel strikes exceed 4 g; 25 Hz is enough for strides of 0.45-2 s; 50 Hz recordings let us check 25 vs 50 Hz on the PC | 400 Hz like the LIS2DS12 code (no benefit, more current) |
| S-04 | 2026-10-30 | INT pins push-pull, active low (CTRL_REG6), MCU pins plain INPUT, falling edge | The board has no pull-ups on INT1/INT2; keeps the old falling-edge handlers; no current through MCU pull-ups | MCU INPUT_PULLUP (old code) |
| S-05 | 2026-10-30 | Count strides of one shoe, report strides x 2 | Device only in the right shoe; each stride is two steps | Counting impacts of both feet (impossible with one sensor) |
| S-06 | 2026-10-30 | Stride = motion burst with a still foot before it, 200-900 ms long, peak >= 450 mg, energy >= 200 g.ms | Walking has stance + long swing; seated taps are short, jiggling/shaking/vehicles have no stance or too little energy. Integer-only (no FPU) | Peak detection on raw magnitude (counts taps and vibration) |
| S-07 | 2026-10-30 | Counting starts after 4 regular strides (8 steps); they are then all counted. A 2 s pause ends the walk | Owner: no steps while the child sits in school. Rhythmic fidgets rarely give 4 valid strides in a row. Cost: walks shorter than 8 steps are not counted | Count every stride (false steps), 2 strides (weaker filter) |
| S-08 | 2026-10-30 | Thresholds are starting values, validated only on synthetic signals; tune from recordings with `step_replay` | No hardware yet. Synthetic tests prove the logic, not the accuracy | Ship untuned |
| S-09 | 2026-10-30 | Work on `feature/lis2dh12-steps` from `main`, only new folders; integration into `stepwhere.ino` after recordings and after PR #1 merges | Handoff rule; `stepwhere.ino` only exists unpacked in PR #1, editing it now would conflict | Branch from `ota/foundation` |
| S-11 | 2026-10-31 | Recordings go to the W25Q128 log area 0x070000-0xFF0000 (256-byte pages with CRC, one session per power-up), downloaded later with `record_steps.py dump` | No cable is possible in the shoe. 15.5 MB = about 14 h at 50 Hz. The area is outside every OTA region (EXT_APP_DATA_ADDR) and below the self-test block | Cable in the shoe; on-board count only (no data to tune) |
| S-12 | 2026-10-31 | On the bring-up build, commands that stop a recording or erase need a confirming `y`; pull-up on USART1 RX | An unplugged RX line can pick up noise bytes | Commands with no confirmation |
| S-13 | 2026-10-31 | w25q driver, ota_layout.h and fake_w25q copied byte-identical from `ota/foundation` | The log needs the flash driver before PR #1 merges; identical files merge without conflict | Second flash driver |
| S-14 | 2026-10-31 | Short / weak motion bursts no longer reset the walk; only too-long bursts or bursts with no stance do | First real walk (recordings/2026-10-31): a small burst (~0.2 s, ~0.8 g) sits between every pair of strides, so v0.3 never reached 4 regular strides and counted 0 | Lower the energy threshold (would let seated fidgets in) |
| S-15 | 2026-10-31 | After a counted walk, 2 regular strides (not 4) restart counting for 10 s | The walk pauses 3-6 s every 5-6 strides (turning); short legs were lost. 110 -> 116 of 122 clear strides | Longer max_stride (keeps counting into the sit-down) |
| S-16 | 2026-10-31 | When a walk resumes after a pause of 1.6-2.7 stride intervals, count one extra stride (the turn) | walk2 (100 steps in a room): every turn is a pivot with no clear swing, 5 per 100 steps; board counted 86. With this: 94 | Lower thresholds to catch the pivot (would admit fidgets) |
| S-17 | 2026-10-31 | Main firmware: LIS2DS12 code replaced by `stepwhere/accel_steps.cpp`. 25 Hz, 10-bit, +-8 g, FIFO read by TIM17 every 500 ms from its interrupt; each detected step does what the old INT2 ISR did (`accel_steps_on_steps`) | loop() blocks for seconds in GSM/GPS waits; the 32-sample FIFO lasts 1.28 s at 25 Hz, so reading from loop() would lose data. Wire works from a priority-3 timer ISR (I2C IRQ 2, SysTick 0). Minute timer, sleep logic and JSON unchanged | Reading in loop(); INT1 watermark ISR (one missed edge stalls it) |
| S-18 | 2026-10-31 | Test build without OTA, linked at 0x08000000, APP_VERSION "v1.2-lis2dh12"; OTA added later | Owner: test the sensor change first, OTA separately | Build on PR #1 |
| S-19 | 2026-11-03 | Keep the FIFO read in the TIM17 ISR (S-17). Guarded: `#error` in `accel_steps.cpp` if the core's I2C / SysTick priority is not more urgent than the timer priority, and the real NVIC values are printed at start | Checked in STM32duino 2.12.0 source: SysTick 0, UART 1, I2C 2, timers 3 (Cortex-M0+). The 17-min log on `main` shows 24.5 Hz, 0 overruns, 0 I2C errors while loop() blocked ~14 s in each of 44 `AT+XIIC=1` calls | Reading in loop() (would overflow the 1.28 s FIFO in those waits) |
| S-20 | 2026-11-03 | The 1-minute timer (TIM16) no longer prints from its ISR. It stores a snapshot; loop() and the AT wait loop print it (`minute_report_service`) | HardwareSerial is not safe from two contexts; the log showed mixed lines (`.+-----`) | Larger TX buffer (only hides it) |
| S-21 | 2026-11-03 | `steps_total` is kept in `.noinit` RAM (magic + inverted copy) across software resets, saved every minute and before the GPS reboot | The GPS reboot reset the total to 0; brown-outs from the loose battery connector would too. `.noinit` lies after `_ebss` and before the heap in the 2.12.0 map (not zeroed) | Writing to the W25Q128 every minute (wear, more code) |
| S-22 | 2026-11-03 | STEPS_DEBUG (default 1 in this test build): per-minute `[STEPS]` line with bursts, candidates, reject reasons, max \|a\|, max motion, gravity. No change to `step_counter.c` | Item 8: 0 steps while walking, 0 overruns, but no way to see why. The bookkeeping is in `step_debug.h`, shared by the firmware and `main_path_replay`; `run_tests.sh` checks it against the counter's own totals | Logging every burst (too much output over hours) |
| S-23 | 2026-11-03 | `tests/motion_host/main_path_replay` replays recordings through the main firmware path (native 25 Hz, normal mode, FIFO watermark 24, 500 ms reads) and runs in `run_tests.sh` | The bring-up path averages 50 Hz pairs; the main firmware does not. Result: same counts (see `docs/STEPS_RESULTS.md`), so the data path is not the cause of item 8 | Only `step_replay --decimate 2` |
| S-10 | 2026-10-30 | Proposed acceptance: walking/running within +-3 % over 100 steps; 0 steps in 5 min sitting; < 10 false steps per school hour | Measurable on recordings; owner asked for "best result, nothing while sitting". Not yet confirmed by the owner | No target |

## Board wiring (Rev10 Gerbers, `Hardware--Stepwhere` branch)

| IC7 pin | Net | MCU |
|---|---|---|
| 1 SCL | 4.7 k pull-up (R23) to IC7 supply | pin 33 PA11 |
| 4 SDA | 4.7 k pull-up (R10) to IC7 supply | pin 34 PA12 |
| 3 SDO/SA0 | series 0 R (R26) | pin 12 PA1 |
| 12 INT1 | direct, no pull | pin 42 PB3 |
| 11 INT2 | direct, no pull | pin 43 PB4 |
| 2 CS, 9 VDD, 10 VDD_IO | IC7 supply = 3.3 V rail through one series part near IC8 | (MCU VDD rail) |
| 6, 7, 8 | GND | |
| 5 | not connected | |

IC1 (TXB0102) is not in the accelerometer path. Designators R10/R23/R26 are
matched from the R11 assembly drawing and the BOM values.

Open hardware items:

* Gerbers are **Rev10** (silkscreen "R10 14.07.2026"); the assembly drawing is
  **R11 (16.08.2026)**. Re-check with R11 Gerbers if the LIS2DH12 boards are R11.
* Value of the series part between 3.3 V and the IC7 supply (bottom side,
  ~3 mm from IC8 pin 1; not on the BOM). 0 R / ferrite / <= 100 R is fine.
  1 k would drop the sensor supply by 0.7-1.4 V whenever I2C pulls low,
  because the pull-ups hang on the same node.
* IC7 pin 5 floats. NC on the LIS2DS12; on the LIS2DH12 it is "Res"
  (reserved), and ST says to connect it to GND
  ([ST community answer](https://community.st.com/mems-sensors-48/what-is-the-res-pin-for-in-the-lis2dh12-mems-accelerometer-83491)).
  The datasheet pin table could not be downloaded from the build sandbox; re-check
  Table 2 of the datasheet before the layout. Rev12 fix in
  `docs/REV12_HARDWARE_FIXES.md`. No rework on Rev11 unless sensor stalls appear.
* The LIS2DS12 (1.62-1.98 V part) ran on 3.3 V on this board, outside its
  operating range. Not an issue for the LIS2DH12 (1.71-3.6 V).
