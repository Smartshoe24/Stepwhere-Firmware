# Handoff: LIS2DS12 → LIS2DH12 software step counter

Context for the chat that replaces the motion IC. Written 2026-10-30 from the
OTA chat. Items marked **CONFIRM** are not verified on the new board.

## 1. Project and rules

* Repo `Smartshoe24/Stepwhere-Firmware`. Work on branch **`feature/lis2dh12-steps` from `main`**.
  Do not touch `ota/foundation` (OTA, PR #1). Main sketch: `stepwhere/stepwhere.ino`
  (on `main` it is still inside `stepwhere.zip`; PR #1 unpacks it).
* Toolchain: Arduino / STM32duino core **2.x** (tested 2.12.0), board "Generic G070CBTx".
  Core 3.0.0 does **not** build the sketch (`HardwareSerial(rx, tx)` changed).
  Libraries: TinyGPSPlus 1.0.3, STM32_TimerInterrupt 1.3.0, STM32duino Low Power 2.0.0, STM32duino RTC 2.0.0.
* Owner works on a Mac: flashes with STM32CubeProgrammer + ST-LINK (SWD), reads the
  debug UART with `python3 tools/ota_uart.py monitor --port /dev/cu.usbserial-0001`
  (CP2102 adapter) or the web bench console. Give prebuilt `.bin` download links and
  exact commands. `screen` / bench console and the script cannot share the port.

## 2. Hardware (GSM PCB, in the heel)

| Function | Pins |
|---|---|
| MCU | STM32G070CBT6: 128 KB flash, 36 KB RAM, 64 MHz |
| Motion IC, old board | LIS2DS12, I2C addr 0x1D (SA0 high) |
| Motion IC, new board | **LIS2DH12** (accelerometer only, no pedometer) |
| I2C | SDA **PA12**, SCL **PA11** (old board) **CONFIRM on new board** |
| Motion SA0 | **PA1** driven HIGH (old) → LIS2DH12 addr would be **0x19** (0x18 if SA0 low) **CONFIRM** |
| Motion INT1 / INT2 | **PB3 / PB4**, input pull-up, falling edge (old) **CONFIRM** |
| Debug UART | USART1 PB6 TX / PB7 RX, 115200 |
| GSM N706B | USART2 PA2/PA3, PWRKEY PB13, LDO_EN PA10 |
| GPS G7A | USART3 PB8/PB9 |
| Battery ADC | PB10 |
| Charger status | PB14 (CHRG), PB15 (STDBY) |
| **Reserved for OTA flash (W25Q128)** | SPI1 **PA5/PA6/PA7**, /CS **PC6**, /WP **PC7**, /HOLD **PB12** — do not reuse |

LIS2DH12 facts to rely on: `WHO_AM_I` (0x0F) = **0x33**; 32-level FIFO; ODR 1–5376 Hz;
low-power / normal / high-resolution modes; two interrupt generators (motion/wake-up);
activity/inactivity function. Check exact register values against the ST datasheet.

## 3. How the current firmware uses steps (LIS2DS12)

`configureLIS2DS12()`: 400 Hz, ±2 g, `FUNC_CTRL = 0x01` (hardware step counter),
step/significant-motion interrupt routed to **INT2**; double tap on INT1 (handler is empty).

* `LIS2DS12_INT2_ISR()`: **one INT2 pulse = one step**. Increments
  `g_steps.step_isr_count`, sets `last_active_ms`, `active`. While `sleep_mode_on`,
  counts `wake_count`; after `MIN_STEPS` (20) steps it clears sleep mode (wake on walking).
* `oneminute_timer_callback()` (HardwareTimer, every `STEP_TIMER_INTERVAL_MS` = 60 s):
  takes `step_isr_count` → `steps_this_min`, adds to `steps_total`; after `IDLE_TIME` (5)
  minutes with **no steps and GPS moved < `WAKE_MIN_DISTANCE_M` (15 m)** → `sleep_mode_on = true`.
* `steps_total` is sent to the server in the location JSON (`build_json`); device status
  "sleep"/"active" comes from `sleep_mode_on`.
* Related constants: `MIN_STEPS 20`, `IDLE_TIME 5`, `STEP_INACTIVITY_LIMIT_MS 300000`,
  `STEP_TIMER_INTERVAL_MS 60000`, `SLEEP_DURATION_S 300`.
* Note: `enter_sleep()` deep-sleep body is inside `#if 0` (currently disabled).

**Simplest integration:** the new code produces the same "step event" that INT2 produced
(increment `g_steps.step_isr_count` etc.), so the minute timer, sleep logic and JSON stay unchanged.

## 4. Constraints from OTA (must keep working)

* App will be linked at **0x08004000**; max app size **112 KB**. Current app ≈ 72 KB →
  keep the step code small (budget ~10 KB). RAM 36 KB total, firmware uses ~4 KB globals.
* During an OTA trial boot the IWDG (~20 s) is running: no blocking loop may exceed ~20 s
  without the main loop running (the app will feed the watchdog in `loop()`).
* Do not use the W25Q128 pins; external-flash region from 0x070000 is free for app data
  if raw recordings need storing on the device.

## 5. Suggested plan

1. Read current code; confirm pins (schematic) and I2C address with an I2C scan + `WHO_AM_I`.
2. LIS2DH12 driver: 25–50 Hz, FIFO (stream mode, watermark interrupt on INT1) so the MCU
   wakes ~1×/s; activity interrupt for wake from sleep on INT2.
3. Recording sketch: stream raw X/Y/Z (CSV) over the debug UART; owner walks known step
   counts.
4. Step algorithm on PC with recordings (filter, adaptive peak threshold, min/max step
   interval ~0.25–2 s, require several regular steps before counting), then on the board.
5. Integrate into `stepwhere.ino` (section 3), measure battery impact.
6. Keep `docs/STEPS_DECISIONS.md` with each decision and why.

Recordings to ask for: 100 steps normal walk, fast walk, running, stairs up/down; and
"should be zero": sitting with foot tapping, car/bike ride, shaking the shoe by hand.

## 6. Open questions for the owner

* New-board pins for SDA/SCL/INT1/INT2/SA0 (schematic).
* One shoe or both? Did the old firmware double the count?
* Target accuracy (e.g. ±5 % over 100 steps)?
* Is double-tap (INT1) used for anything?
