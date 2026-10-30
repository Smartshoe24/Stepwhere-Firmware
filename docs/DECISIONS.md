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

## Follow-ups noted (not OTA, tracked here so they are not lost)

* Location uploads go over plain HTTP; move to **HTTPS** (children's location data). Handled in the firmware-fixes work.
* After a cable reflash the metadata version can be stale (seen: shows v1.1.0 while v1.0.0 runs). Fix: bootloader compares stored SHA-256 with the app; app reports its compiled-in version.
* Production programming must use a full chip erase (old data inflated the first backup to 58 KB).
* Unexplained resets seen on the bench board a few times: check power / battery connector / brown-out.
* LIS2DH12 replaces LIS2DS12 (no hardware pedometer): software step counter, separate task.
