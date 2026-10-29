# OTA bench test files

Built by `tests/ota_test_app/build.sh`. Test firmware only, not for products.

| File | Use |
|---|---|
| `bootloader_testapp_v1.0.bin` | Flash at **0x08000000** with STM32CubeProgrammer (bootloader + test app v1.0) |
| `testapp_v1.1.ota` | Good update. Must install and confirm. |
| `testapp_v1.2_noconfirm.ota` | Never confirms, resets every 10 s. Bootloader must roll back to v1.1. |
| `testapp_v1.3_corrupt.ota` | One byte damaged. Must be rejected, device stays on its version. |

## Sending files (Mac / Linux / Windows)

```sh
pip3 install pyserial
python3 tools/ota_uart.py ports                                   # find the port
python3 tools/ota_uart.py monitor --port /dev/cu.usbserial-XXXX   # terminal, keys i/u/r/e/h
python3 tools/ota_uart.py upload  --port /dev/cu.usbserial-XXXX prebuilt/ota_test/testapp_v1.1.ota --reset
```

On Windows, Tera Term also works: press `u`, then File > Transfer > XMODEM > Send, tick **1K**.
