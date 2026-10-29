# Stepwhere firmware

Firmware for the Stepwhere GSM PCB (STM32G070CBT6 + Neoway N706B + W25Q128).

```
stepwhere/              main application sketch (Arduino, STM32duino core 2.x)
libraries/StepwhereOTA/ OTA building blocks: W25Q128 driver, OTA layout
tests/flash_selftest/   sketch that checks the external flash on a board
tests/host/             unit tests that run on a PC (no hardware needed)
tools/ota_tool.py       packs and signs firmware images for OTA
docs/OTA_DESIGN.md      OTA memory map, image format and boot flow
```

## Building

Board settings (Arduino IDE → Tools):

* Board: **Generic STM32G0 series**
* Board part number: **Generic G070CBTx**
* Core: **STMicroelectronics stm32 2.x** (3.0.0 does not build this sketch)

Libraries: TinyGPSPlus, STM32_TimerInterrupt 1.3.0, STM32duino Low Power,
STM32duino RTC, and `libraries/StepwhereOTA` from this repo (copy the folder into
your Arduino `libraries` folder, e.g. `Documents/Arduino/libraries/`).

With `arduino-cli`:

```sh
FQBN=STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX

# Flash self-test (normal build at 0x08000000)
arduino-cli compile --fqbn $FQBN --libraries libraries --output-dir build/selftest tests/flash_selftest

# Application linked at 0x08004000 (for use with the OTA bootloader, not yet available)
arduino-cli compile --fqbn $FQBN --build-property build.flash_offset=0x4000 --output-dir build/app stepwhere
```

## Flash self-test

1. Pins are set from the schematic: SPI1 on PA5/PA6/PA7, /CS PC6, /WP PC7,
   /HOLD PB12.
2. Build it and flash `flash_selftest.ino.bin` (or `.hex`) with STM32CubeProgrammer
   as usual, at address `0x08000000`.
3. Open the debug UART (USART1: PB6 TX, PB7 RX, 115200 baud). It prints each
   step and `RESULT: ALL TESTS PASSED` or `RESULT: FAILED`. Press any key to run
   it again.

It only erases and writes the last 64 KB of the flash, outside all OTA areas.

## Host tests

```sh
pip install cryptography
sh tests/host/run_tests.sh
```
