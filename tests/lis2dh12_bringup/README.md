# LIS2DH12 bring-up and step recordings

`lis2dh12_bringup.ino` checks the accelerometer (IC7) on a GSM PCB and records
raw motion data so the step counter can be tuned on real walking.

Prebuilt: [`prebuilt/lis2dh12_bringup.bin`](../../prebuilt/lis2dh12_bringup.bin)
(standalone, linked at 0x08000000).

## 1. Flash

STM32CubeProgrammer + ST-LINK (SWD), address **0x08000000**:

```sh
STM32_Programmer_CLI -c port=SWD -w prebuilt/lis2dh12_bringup.bin 0x08000000 -v -rst
```

This replaces the bootloader and the app on that board. To go back to normal
firmware, flash the factory image (bootloader + app) again.

## 2. Board check

```sh
python3 tools/ota_uart.py monitor --port /dev/cu.usbserial-0001
```

Let the board lie still. After reset it prints:

```
=== Stepwhere LIS2DH12 board check (lis2dh12_bringup v0.1) ===
I2C devices: 0x19
[PASS] I2C scan finds a device
[PASS] WHO_AM_I = 0x33 at 0x19 - 0x19 (PA1 high): answers 0x33
[PASS] SA0 follows PA1 (0x18 when low) - ...
[PASS] configure 50 Hz, FIFO, INT1 - OK
[PASS] FIFO delivers samples - 100 samples in 2 s (expect ~100)
[PASS] |a| at rest 900..1100 mg - ...
[PASS] INT1 watermark interrupt - ...
       Keep still 2 s ...
       Now MOVE or tap the board (10 s) ...
[PASS] INT2 motion interrupt - ...
[PASS] INT2 quiet when still
RESULT: ALL CHECKS PASSED
```

Move or tap the board when it asks. Send the whole output, PASS or FAIL.
`t` runs the check again. Quit the monitor (Ctrl-C) before recording: the
recorder needs the port.

## 3. Recordings

The board must be **in the right shoe, in its final position in the heel**,
on battery, with the USB-serial cable to a laptop that the walker carries (or
a long cable). Count steps aloud or with a clicker.

```sh
pip3 install pyserial
python3 tools/record_steps.py --port /dev/cu.usbserial-0001 --label walk100
```

Press **Enter** to drop a marker: once just before the first step and once
after the last. Ctrl-C stops. Files go to `recordings/`. Write the real step
count in the label or tell us.

Please record (each with markers around the activity):

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

Upload the CSV files to the repo (folder `recordings/`) or send them.

The board also prints `#steps N` from the on-board counter while recording,
so you can see roughly how it does. Real tuning happens on the PC:

```sh
sh tests/motion_host/run_tests.sh                     # builds step_replay
tests/motion_host/build/step_replay --decimate 2 -v recordings/xxx_walk100.csv
tests/motion_host/build/step_replay --decimate 2 --set high_mg=500 recordings/xxx_sit_taps.csv
```

## Build from source

```sh
arduino-cli compile --fqbn STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX \
    --libraries libraries --output-dir build/bringup tests/lis2dh12_bringup
```

STM32duino core 2.x (tested 2.12.0). Only the `StepwhereMotion` library from
this repo is needed.
