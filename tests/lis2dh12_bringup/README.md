# LIS2DH12 bring-up and step recordings

`lis2dh12_bringup.ino` checks the accelerometer (IC7) on a GSM PCB and
records motion data into the board's own W25Q128 flash, so the board can be
in the shoe **without a cable**. You download the recordings afterwards.

Prebuilt: [`prebuilt/lis2dh12_bringup.bin`](../../prebuilt/lis2dh12_bringup.bin)
(v0.5, standalone, linked at 0x08000000).

## 0. Get the tools (once, on the Mac)

The commands below assume a folder with the two scripts in it. No clone needed:

```sh
mkdir -p ~/stepwhere && cd ~/stepwhere
curl -LO https://raw.githubusercontent.com/Smartshoe24/Stepwhere-Firmware/ota/foundation/tools/ota_uart.py
curl -LO https://raw.githubusercontent.com/Smartshoe24/Stepwhere-Firmware/feature/lis2dh12-steps/tools/record_steps.py
curl -LO https://github.com/Smartshoe24/Stepwhere-Firmware/raw/feature/lis2dh12-steps/prebuilt/lis2dh12_bringup.bin
pip3 install pyserial
```

Run the scripts from that folder as `python3 ota_uart.py ...` and
`python3 record_steps.py ...` (no `tools/` in front).

## 1. Flash

STM32CubeProgrammer + ST-LINK (SWD), address **0x08000000**:

```sh
STM32_Programmer_CLI -c port=SWD -w lis2dh12_bringup.bin 0x08000000 -v -rst
```

This replaces the bootloader and the app on that board. To go back to normal
firmware, flash the factory image (bootloader + app) again. The OTA areas of
the external flash are not touched (the log uses 0x070000-0xFF0000).

## 2. Board check (cable connected, board lying still, on battery)

```sh
python3 ota_uart.py monitor --port /dev/cu.usbserial-0001
```

Press reset (or power-cycle). It prints:

```
=== Stepwhere LIS2DH12 board check (lis2dh12_bringup v0.5) ===
I2C devices: 0x19
[PASS] I2C scan finds a device
[PASS] WHO_AM_I = 0x33 at 0x19 - ...
[PASS] SA0 follows PA1 (0x18 when low) - ...
[PASS] configure 50 Hz, FIFO, INT1 - OK
[PASS] FIFO delivers samples - ...
[PASS] |a| at rest 900..1100 mg - ...
[PASS] INT1 watermark interrupt - ...
[PASS] external flash for the log - W25Q128 ID EF 4018, log OK
RESULT: ALL CHECKS PASSED
#RECORDING session 1 to flash. You can unplug the cable now.
#info RECORDING, session 1, log 0/63488 pages (0.0%), 14 h 6 min left, steps 0
```

**First time on each board**, erase the log area once: press `E`, then `y`
(1-8 minutes). Then press `t`, then `y`, for the full check. It also tests
INT2: move or tap the board when it asks. Send the whole output, PASS or FAIL.

## 3. Recording in the shoe (no cable)

* **Every power-up or reset starts a new session** and recording begins
  about 3 s after the board check.
* The board records the whole time it has power: 50 Hz, about 14 hours of
  space. The battery must be charged. Without GSM/GPS it lasts many hours.
* Unplug the cable (TX/RX/GND), then put the board in the right shoe in its
  final position.

Write down what you do, with times from a phone stopwatch started when you
unplug the cable. If you can power-cycle the board (battery connector),
start a new session for each activity. That makes the files easiest to use.
Either way, **stand still for 10 s before and after each activity**: the
still parts separate the activities in the data.

| Label | What | Real steps |
|---|---|---|
| `walk100` | normal walk, 100 steps (count both feet) | 100 |
| `fastwalk100` | fast walk, 100 steps | 100 |
| `run100` | running, 100 steps | 100 |
| `stairs_up`, `stairs_down` | one flight each | count |
| `child_walk` | the child walking, if possible | count |
| `short_walks` | 5 times: 4 steps, stop 5 s | 20 |
| `sit_taps` | sitting, tapping the foot, 3 min | 0 |
| `sit_jiggle` | sitting, jiggling the leg, 3 min | 0 |
| `sit_swing` | sitting, swinging the foot under the chair, 3 min | 0 |
| `sit_stomp` | sitting, lifting the heel and stomping, 2 min | 0 |
| `car` / `bus` / `bike` | 10 min ride, shoe on the foot | 0 |
| `shake` | shaking the shoe by hand, 1 min | 0 |
| `school_day` | a normal day at school, if possible | (the step count from the old device or a phone, if any) |

Example notes: `0:30-1:40 walk100 (100) | 2:00-5:00 sit_taps (0) | ...`

## 4. Download

Take the board out, connect the cable, then run (not at the same time as the monitor):

```sh
pip3 install pyserial
python3 record_steps.py dump --port /dev/cu.usbserial-0001 --label day1
```

You get one file per session in `recordings/`, and the board's step count
for each session:

```
  recordings/20261031_101500_day1_s1.csv:  42.3 min, 126900 samples, 0 gaps, board counted 214 steps
```

Downloading takes about 5 minutes per hour of recording. Add `--erase` to
clear the board after a complete download. Then the board starts a new
session and records again. Upload the CSV files plus your notes (folder
`recordings/` in the repo, or send them).

## 5. Keys (in the monitor)

| Key | What |
|---|---|
| `i` | info: session, space used, live step count |
| `d` | stop recording and download (use `record_steps.py dump` instead to save files) |
| `n` | start a new session |
| `x` | stop recording |
| `e` / `E` | erase the recorded sessions / the whole log area |
| `t` | full board check, including INT2 |
| `r` | live CSV over the cable (`record_steps.py live`), `m` marker, `s` stop |
| `h` | help |

While it records, and for `e`/`E`, the board asks `send y to confirm`.
Noise on an unplugged cable therefore cannot stop a recording or delete it.

## Tuning on the PC

```sh
sh tests/motion_host/run_tests.sh                     # builds step_replay
tests/motion_host/build/step_replay --decimate 2 -v recordings/xxx_s1.csv
tests/motion_host/build/step_replay --decimate 2 --set high_mg=500 recordings/xxx_s1.csv
```

## Build from source

```sh
arduino-cli compile --fqbn STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX \
    --libraries libraries --output-dir build/bringup tests/lis2dh12_bringup
```

STM32duino core 2.x (tested 2.12.0). Needs the `StepwhereMotion` and
`StepwhereOTA` libraries from this repo.
