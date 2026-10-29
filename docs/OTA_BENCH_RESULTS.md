# OTA bench test results

Board: GSM PCB with STM32G070CBT6 and W25Q128 (JEDEC EF 4018, always-on 3.3 V).
Bootloader v0.1.0, test app from `tests/ota_test_app`, files from `prebuilt/ota_test`.
Uploads over the debug UART with `tools/ota_uart.py` (XMODEM-1K) on macOS.

| # | Test | Result |
|---|---|---|
| 0 | Flash self-test | PASS: all steps; sector erase 34 ms, 64 KB erase ~175 ms |
| 1 | Bootloader first boot, jump to app | PASS: no metadata, app starts, VTOR = 0x08004000 |
| 2 | Update v1.0 -> v1.1 | PASS: backup, install, trial boot 1, app confirms, state IDLE |
| 3 | Unconfirmed v1.2 | PASS: trial boots 1-3, rollback to v1.1 from backup on boot 4 |
| 4 | Corrupt v1.3 | PASS: rejected by SHA-256 in the app, no metadata write (seq unchanged) |
| 5 | Power cuts during install | PASS: cut during backup 3 times, resumed from BACKING_UP, installed and confirmed |
| - | Abandoned transfer (no sender) | PASS: app times out after 60 s and keeps running |

Notes
* A cable-flashed app has no recorded version, so its backup shows v0.0.0.
* After a rollback the failed image stays in the download slot with state IDLE;
  the app must remember not to download it again.
* Several unexplained resets were seen during testing (self-test, step 1, after
  step 5). To check: power wiring / battery connector, brown-out.
