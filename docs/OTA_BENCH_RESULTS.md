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

## Bootloader v0.2.0 (signature + watchdog)

| # | Test | Result |
|---|---|---|
| A | Signed update v1.1 | PASS: signature ok (app 531 ms at 64 MHz, bootloader 1693 ms at 16 MHz, checked twice), watchdog on during trial, app confirms and restarts, then watchdog off |
| B | Hanging v1.4 | PASS: watchdog reset 3 times, rollback to v1.1 (backup hash check 425 ms), restart, watchdog off |
| C1 | Unsigned v1.5, forced PENDING | PASS: app and bootloader both reject (not signed), restart, v1.1, last result "rejected bad image" |
| C2 | Wrong-key v1.6, forced PENDING | PASS: app and bootloader both reject (bad signature), restart, v1.1 |

Notes
* Without metadata the bootloader backs up up to the last non-0xFF byte of the
  app area; old data left by STM32CubeProgrammer (it only erases what it writes)
  made the first backup 58,520 bytes. Use a full chip erase in production.

## Bootloader v0.2.1 (production key 59C07275)

| # | Test | Result |
|---|---|---|
| P1 | v1.1 signed on the key owner's Mac | PASS: signature ok (1704 ms), install, confirm, restart, watchdog off |
| P2 | v1.5 signed with the old dev key, forced PENDING | PASS: app and bootloader reject (bad signature), restart, app unchanged |

Open issue found
* After re-flashing the app by cable, the metadata still said "installed v1.1.0"
  while v1.0.0 was running. The bootloader trusts the stored version. Fix planned:
  compare the stored SHA-256 with the app in flash when backing up, and the app
  should report its own compiled-in version to the server.

## New board, OTA test kit run 2026-11-05 (bootloader v0.2.2)

T01-T09, T12: PASS (signature check 1.81 s, watchdog reset at 20 s).
T10 power cut: SKIP (install finished before the battery was pulled, 3 tries;
prompt now shown earlier). T11: not run (no ST-LINK during test).
T13 modem: N706B-A07-STD-BZ_CN1X-008, SIM ready, CSQ 21, registered (CEREG 0,1),
AT+XIIC=1 OK. Probe bug: it stopped reading at "+XIIC:" so the IP was never
seen; fixed (wait for OK).

## Real firmware over LTE, 2026-11-09 (bootloader v0.3.0, key 59C07275)

Tracker firmware v2.1.x (not the test app), served by device-ingest-proxy at
`ingest.stepwhere.in/fw/` as hex (`?enc=hex`, 512 B per request). Files and
steps: `tests/ota_real_test/`. Board IMEI 869938082392680, the only device.

| # | Test | Result |
|---|---|---|
| L0 | v2.1.0 -> 2.1.1 over LTE | PASS: 99,140 bytes in ~5 min between uploads, survived a modem power cycle mid-download, confirmed on the first 201 |
| R1 | v2.1.1 -> 2.1.2 (fixes D-33, D-34) | PASS: installed, trial boot 1, confirmed on the first 201 |
| R2 | Battery pulled mid-download (2x, at 45,056 and 28,672 bytes) | PASS: metadata untouched (seq 47), v2.1.1 kept running, download restarted from 0 (progress is RAM only, by design) |
| R3 | Battery pulled at `watchdog on` | PASS (partial): cut landed in PENDING before the backup started; resumed, backed up, installed, confirmed. A cut during BACKING_UP / INSTALLING is not yet done on the real firmware |
| R4 | Damaged 2.1.3 (signed, 1 byte flipped) | PASS: `REJECTED: SHA-256 mismatch`, no restart; next check `skipped: image rejected before` (D-34) |
| R5/R6 | Dev-key 2.1.4 / unsigned 2.1.5 | SKIPPED: same check as R4, passed earlier on the test app (C1, C2, P2) |
| R7 | Never-confirming 2.1.6 (limit 3 min for the test) | PASS: restarted itself after 3 min online (D-33), trial boots 2 and 3, rolled back to 2.1.2 (`last rolled_back`); next check `skipped: rolled back before` (D-32) |
| R8 | Hanging 2.1.7 | PASS: watchdog reset 3x (`Reset cause: independent-watchdog`), `restored v2.1.2 from backup slot` (hash check 1374 ms), restart, running 2.1.2 |

Timings: bootloader signature check 2.66-2.70 s (twice per install), app
backup ~99 KB.

Notes
* Plugging the battery in gives 2-7 quick brown-out boots (connector bounce),
  some cut off mid-line. Harmless (metadata unchanged) and the likely cause of
  the earlier "unexplained resets".
* Not OTA: LIS2DH12 not responding on this board; GPS sends no data, which
  power-cycles the modem every ~2 min; Wi-Fi scans see 1 AP (2 needed).
* Test versions 2.1.3-2.1.7 were offered, so the next release must be >= 2.2.0.
