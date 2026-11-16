---
inclusion: always
---
# Stepwhere tracker firmware: project context (handoff, 2026-11-15)

Read this first in a new session. Also read `git-push-policy.md` (no push
without "PUSH to github main").

## Working with the owner
- The owner (Shubham, GitHub `Smartshoe24`) is **not a developer**. Explain in
  short, plain language; give exact click-by-click or copy-paste steps.
- He runs everything on hardware, AWS, Infisical and GitHub settings himself;
  he has no reset button on the board (battery pull = reset).
- His Mac: this repo is cloned at `~/Downloads/Stepwhere-Firmware-ota-test`
  (branch may be old: `git pull` first). Signing key: `~/stepwhere-keys/ota_signing_key.pem`.
  Serial monitor: `python3 tools/ota_uart.py monitor --port /dev/cu.usbserial-0001`.

## Related repos
- **Server, admin, QA bench, QA firmware: `Smartshoe24/stepwhere-stack-new`** (live,
  deploys to production). See its `.kiro/steering/`.
- `stepwhere/stepwhere-stack` is the **old server repo: retired**, it can no longer
  deploy (AWS trusts only the new repo). Do not make changes there.
- `apps/firmware/stepwhere.ino` in stepwhere-stack-new is an **older copy** of the
  tracker; the production tracker is `stepwhere/` in **this** repo.

## Hardware (Rev11 board, "STEPWHERE R11 16.08.2026")
STM32G070CBT6, Neoway N706B LTE modem (also does Wi-Fi scan), W25Q128 SPI flash
(always-on 3.3 V), LIS2DH12 accelerometer (I2C PA12/PA11), G7A GPS (UART PB8/PB9),
battery sense PB10 via 1:2 divider. Bench board and its faults:
`docs/handoff/BOARD_REV11_BENCH.md`.

## Firmware now
- Production firmware **v2.2.0-lis2dh12** = `stepwhere-final-v2.1-lis2dh12.zip`
  (GSM, GPS, LIS2DH12, Wi-Fi/GSM location fallback) + OTA (`stepwhere/ota.ino`).
  Released to S3 on 2026-11-10 and installed over the air on the bench board.
- Bootloader **v0.3.0** at 0x08000000 (16 KB), app at 0x08004000. Trusts only the
  production key, fingerprint **59C07275**.
- **Next version must be ≥ 2.2.1** (2.1.2–2.1.7 were OTA test builds, 2.2.0 is live).
- Planned 2.2.1: add the STM32 chip UID to the `fw` block (`fw.uid`, 24 hex), so
  the server can auto-register production-firmware boards in QA.

## OTA (all verified on hardware, see `docs/OTA_BENCH_RESULTS.md`, decisions D-01..D-34 in `docs/DECISIONS.md`)
- Device checks `http://ingest.stepwhere.in/fw/devices/<IMEI>.json`, then
  `/fw/manifest.json`, 3 min after boot and then daily (debug UART `o` = now,
  `O` = status). Download 512 B per request as hex (`?enc=hex`): the N706B drops
  binary HTTP replies.
- The ingest proxy reads the files from CloudFront/S3 and hex-encodes them.
- New firmware confirms on the first accepted upload (HTTP 201); not confirmed
  after 30 min online → restarts; rollback after 3 trial boots; a rolled-back or
  rejected version is not downloaded again.
- **Not yet tested on the real firmware:** power cut during BACKING_UP / INSTALLING.
- Known limit: a tracker that sleeps (no steps ~5 min, wakes at ≥ 20 steps/min,
  no timer wake) never checks for updates while asleep.

## Releasing (docs/RELEASING.md)
1. Set `#define APP_VERSION "v2.2.1-lis2dh12"` in `stepwhere/stepwhere.ino`, merge to main.
2. Push tag `v2.2.1` (only after "PUSH to github main").
3. Owner approves in GitHub Actions → "Release firmware" → Review deployments.
   Workflow builds, signs (env secret `OTA_SIGNING_KEY`), checks 4 gates, uploads
   image then manifest to S3, creates a GitHub Release with `stepwhere_factory.hex`.
- GitHub environment `firmware-release`: reviewer Smartshoe24, tag rule `v*`,
  vars FIRMWARE_BUCKET / FIRMWARE_DISTRIBUTION_ID / AWS_PUBLISH_ROLE_ARN.
- AWS (account **713881826913**, the management account, ap-south-1): CloudFormation
  stack `stepwhere-firmware` from `infra/aws/firmware-hosting.yaml`; bucket
  `stepwhere-firmware-713881826913`; CloudFront `d1e6333g4g5fxj.cloudfront.net`
  (E2SFB6DZ5ELIBK); role `stepwhere-firmware-publish` trusts the immutable OIDC
  subject `repo:Smartshoe24@166411271/Stepwhere-Firmware@1437508917:environment:firmware-release`.
  Template changes are applied by the owner in CloudShell (`aws cloudformation deploy …`).
- New boards: flash `stepwhere_factory.hex` from the GitHub Release with a full chip erase.

## Build and test
arduino-cli, core STMicroelectronics:stm32@2.12.0 (core 3.x breaks HardwareSerial).
`sh tools/ci_build.sh` builds everything; `sh tests/host/run_tests.sh` host tests
(needs Python `cryptography`). Real-firmware OTA test kit: `tests/ota_real_test/`.

## Backlog (bring up when the owner asks "what to build next")
- Own Wi-Fi/cell location database learned from GPS fixes, to replace most Google
  Geolocation calls (saved learning).
- 2.2.1 with chip UID. Power-cut-during-install test on real firmware.
