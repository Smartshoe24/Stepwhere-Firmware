# Stepwhere OTA: decision log

Why the OTA system is built the way it is. Add a row whenever a decision is
made; never delete one, mark it "Replaced by D-xx" instead.

| ID | Date | Decision | Why | Alternatives rejected |
|---|---|---|---|---|
| D-01 | 2026-10-28 | Custom bootloader, not MCUboot | 128 KB single-bank MCU; MCUboot is heavy to port on bare-metal STM32 and we need full control of the W25Q layout | MCUboot |
| D-02 | 2026-10-28 | Internal flash: 16 KB bootloader at 0x08000000, app at 0x08004000 (112 KB) | App is ~73 KB; leaves ~39 KB growth. Bootloader is 12.3 KB incl. ECDSA | 24/32 KB bootloader (wastes app space) |
| D-03 | 2026-10-28 | Copies live in external W25Q128: download, backup, factory slots (128 KB each); A/B metadata sectors | Two app copies do not fit in 128 KB internal flash; 16 MB external is plenty | Dual-bank internal (impossible, single bank) |
| D-04 | 2026-10-28 | Every state saved before its step; install/backup/rollback repeatable | Battery device, power cuts likely. Verified: 397 + 97 cut points on PC, 3 real cuts on board | Single "busy" flag |
| D-05 | 2026-10-28 | New app gets 3 boots to call ota_meta_confirm(), else rollback | Catches firmware that crashes or never reaches its self-test | No rollback |
| D-06 | 2026-10-28 | Keep app on Arduino/STM32duino core 2.x; bootloader in plain C, register level | Existing firmware is Arduino; core 3.0.0 breaks HardwareSerial(rx,tx); HAL would not fit 16 KB | Rewrite app in CubeIDE |
| D-07 | 2026-10-29 | W25Q128 pins: SPI1 PA5/PA6/PA7, /CS PC6, /WP PC7 and /HOLD PB12 driven HIGH; flash on always-on 3.3 V | From schematic; verified by self-test (EF 4018). /WP and /HOLD are MCU pins, floating /HOLD would stall reads | — |
| D-08 | 2026-10-29 | Images: 256-byte header (version, size, hw_id, SHA-256, signature, CRC) + raw app | Lets app and bootloader reject wrong board, wrong link address, damage | Raw .bin |
| D-09 | 2026-10-30 | ECDSA P-256 signature over the header (which holds the payload SHA-256), micro-ecc | Only our firmware can be installed even if the server or transport is compromised. Verify 1.7 s in bootloader, 0.5 s in app | No signing; HMAC (shared secret would sit in every device) |
| D-10 | 2026-10-30 | Production key generated on the owner's Mac only (fingerprint 59C07275); private key never in repo or chat; offline backup required | Whoever holds it controls every device; losing it means no more updates | Key generated on build server |
| D-11 | 2026-10-30 | Watchdog (IWDG ~20 s) only during install/trial/rollback; app restarts once after confirm | Catches hanging firmware (verified) without affecting long deep sleep in normal operation | Always-on watchdog (would wake/reset during 5-min sleeps) |
| D-12 | 2026-10-30 | Bench test with a stand-in test app, not the main firmware | Main firmware has open bugs; keeps bootloader results unambiguous | Test with main firmware |
| D-13 | 2026-10-30 | Host firmware on AWS: private S3 + CloudFront, region ap-south-1, published by GitHub Actions via OIDC role (tags v* only) | Company already uses AWS; no stored AWS keys; cents per month; stable URL | Own server, GitHub Releases (redirects, rate limits, token on device) |
| D-14 | 2026-10-30 | Devices download firmware over plain HTTP | Signature already blocks tampering; avoids CA certificate expiry bricking downloads fleet-wide; less battery/data | HTTPS on N706B |
| D-15 | 2026-10-30 | Device URL is fw.stepwhere.in (CNAME at GoDaddy → CloudFront) | Storage can move without a firmware change | Hard-coding the cloudfront.net name |
| D-16 | 2026-10-30 | Device checks manifest.json once a day, only when GSM is already on | Near-zero extra battery; no backend change. Server-pushed trigger can come later for staged rollouts | Server flag in location reply (later option) |

| D-17 | 2026-10-30 | Probe the N706B download methods on hardware before writing the downloader | Firmware only ever POSTed; no AT manual available; resume needs byte ranges | Guess from other Neoway modules |
| D-18 | 2026-10-30 | CI on every push (host tests incl. sanitizers, all builds); release only on v* tags, toolchain pinned (core 2.12.0, libs) | Reproducible builds; tag = deliberate release | Release on every main push |
| D-19 | 2026-10-30 | Release gates: tag == APP_VERSION, firmware calls ota_meta_confirm(), signature verifies with committed production public key, version newer than published, human approval in GitHub environment | Each one prevents a fleet-wide failure (rollback loop, wrong key, downgrade, accident) | Unattended publish |
| D-20 | 2026-10-30 | Signing key stored only as a GitHub environment secret (firmware-release) + offline backup | Secret usable only by approved tag runs, never readable again | Repo-level secret, key on a build PC |
| D-21 | 2026-10-30 | Upload image first, manifest last; manifest cached 60 s, images immutable | Devices never see a manifest pointing at a missing file | Single upload order-free |
| D-22 | 2026-10-30 | Local update server (tools/fw_server.py on the Mac + tunnel) first, AWS later | Faster to test; same layout as S3 (manifest.json, releases/<v>/stepwhere.ota), so moving to AWS only changes the device base URL. Server supports byte ranges like CloudFront | Deploy AWS first |
| D-23 | 2026-10-30 | OTA layout uses only the first 448 KB of external flash; driver uses only standard 25-series SPI NOR commands | Any 3.3 V SPI NOR of >= 4 Mbit can replace the W25Q128 with an ID-table + timeout change, same firmware for mixed boards (stock risk). Avoid SPI NAND / I2C EEPROM / 1.8 V parts | Chip-specific features (quad, security registers) |
| D-24 | 2026-10-30 | Targeted updates (planned): device first reads devices/<IMEI>.json, then manifest.json; manifest may carry "rollout": N (% of devices by IMEI hash) | Test on chosen devices and staged rollouts with static files, no backend code; signature still required | Separate firmware per group |
| D-25 | 2026-10-30 | Fleet monitoring: a "fw" block (app, boot, key, ota, last, avail, dl) added to the EXISTING location upload; admin shows versions per device | No extra connection or battery; agreed with owner. Admin-controlled rollouts come after monitoring | Separate /device-status endpoint |
| D-26 | 2026-10-30 | Bootloader gets a fixed info block (version, key fingerprint) the app can read | So the app can report the bootloader version and trusted key | App guessing the bootloader version |
| D-27 | 2026-11-06 | Main firmware = v2.1-lis2dh12 (Stepwhere-Firmware); OTA added as a separate tab stepwhere/ota.ino with 6 small hooks in stepwhere.ino | Keeps the tracker code readable; later firmware changes rarely touch OTA | Rewrite into stack apps/firmware v2.0 |
| D-28 | 2026-11-06 | Trial firmware confirms after the first upload the server accepts (201); no restart after confirm, watchdog stays fed from service_background() | 201 proves modem, data and server path work; avoids a modem power cycle. Every wait in v2.1 already goes through service_background() | Confirm on a timer; restart after confirm (as test app) |
| D-29 | 2026-11-06 | Download over AT+HTTPACTION=99 (own GET with Range) as hex text (`?enc=hex`, 512 B = 1024 chars per request: live probe 2026-11-09 showed the N706B passes text replies but drops binary ones), earlier 1 KB per request (was 4 KB: the live board received only 2341 of 4096 bytes, UART RX buffer is 1 KB), sector erased before its first part, other jobs run only while the modem line is quiet; parser accepts +HTTPRECV with or without length | Matches how v2.1 uploads; resumable; retries cannot hit non-erased flash. Framing seen on hardware: \r\nOK\r\n\r\n+HTTPRECV: <HTTP response>\r\n+HTTPCLOSED | TCP sockets (TCPSETUP failed in probe) |
| D-30 | 2026-11-06 | Bootloader v0.3.0 exposes an info block at 0x080000C0 (version, key fingerprint, hw id) | App reports bootloader + key in the fw block (D-26) | — |
| D-31 | 2026-11-06 | fw block omitted when a large Wi-Fi payload leaves < 220 bytes; server schema strips unknown keys so old servers accept it | An oversize payload would skip the whole upload | Always append |
| D-32 | 2026-11-06 | Device never re-downloads a version it rolled back from (read from the download-slot header) | Avoids a download/install/rollback loop | Server-side blocklist only |
| D-33 | 2026-11-09 | Trial firmware not confirmed after 30 min with a data connection restarts itself; time without a connection does not count | Without it a firmware that runs but never gets a 201 stays unconfirmed (and blocked from updates) until someone resets the board; each restart uses one of the 3 trial boots, so it rolls back on its own. Not counting offline time keeps a good firmware from rolling back in a basement or on low battery (D-32 would then block it for good) | Plain 30 min timer (rolls back good firmware without coverage); bootloader-side timer (watchdog cannot measure uptime) |
| D-34 | 2026-11-09 | A version whose downloaded image fails the SHA-256/signature check is skipped until the next restart | Each part is read back after writing, so a failed check means the file is bad; re-downloading every 30 min cost ~5 MB/day per device. One retry per restart still recovers from a rare one-off fault | Remember across restarts (would need a flash write); keep retrying |

## Follow-ups noted (not OTA, tracked here so they are not lost)

* Location uploads go over plain HTTP; move to **HTTPS** (children's location data). Handled in the firmware-fixes work.
* ~~Stale version after cable reflash~~: fixed in bootloader v0.2.2 (compares stored SHA-256 with the app before backup). Apps should still report their compiled-in version to the server.
* Production programming must use a full chip erase (old data inflated the first backup to 58 KB).
* Unexplained resets seen on the bench board a few times: check power / battery connector / brown-out.
* Admin/backend repo access pending (owner shares later): implement storage and views for the fw block (D-25).
* LIS2DH12 replaces LIS2DS12 (no hardware pedometer): software step counter, separate task.
