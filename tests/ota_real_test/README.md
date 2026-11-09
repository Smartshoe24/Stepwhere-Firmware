# OTA tests with the real tracker firmware

Same board as the LTE test of 2026-11-09 (bootloader v0.3.0, key 59C07275,
running v2.1.1). Each test is one `manifest.json` on `ingest.stepwhere.in/fw/`;
press `o` on the debug UART to check at once instead of waiting 3 minutes.

## Files

`build.sh` makes `prebuilt/ota_real_test/app_bins/` and the two reject files.
`sign_with_my_key.sh` makes the production-signed files on the key owner's Mac:

```sh
cd Stepwhere-Firmware
git fetch && git checkout fw/v2.1-ota && git pull
sh tests/ota_real_test/sign_with_my_key.sh ~/stepwhere-keys/ota_signing_key.pem
```

Then upload `prebuilt/ota_real_test/releases/` (6 folders, 2.1.2 to 2.1.7) to
stepwhere-stack `apps/device-ingest-proxy/firmware/releases/`.

| Version | File | Built from |
|---|---|---|
| 2.1.2 | good | `stepwhere/` with both fixes (D-33, D-34) |
| 2.1.3 | signed, one byte damaged | 2.1.2 binary |
| 2.1.4 | signed with the old development key | 2.1.2 binary |
| 2.1.5 | not signed | 2.1.2 binary |
| 2.1.6 | never confirms; restarts after **3 min** online (release: 30) | `-DOTA_TEST_NO_CONFIRM=1` |
| 2.1.7 | hangs at its trial boot | `-DOTA_TEST_HANG=1` |

After these tests the next real release must be **2.2.0 or higher**.

## Tests

| # | Manifest | Do | Pass when the log shows |
|---|---|---|---|
| R1-R3 | 2.1.2 | `o`; at about half of the download press reset; after the restart `o` again; when `watchdog on` appears pull the battery, wait 2 s, put it back | download starts again from 0 after the reset; bootloader continues from `BACKING_UP` or `INSTALLING`; `trial boot 1 of 3`; `New firmware confirmed` |
| R4 | 2.1.3 | `o`, wait for the download; then `o` again | `REJECTED: SHA-256 mismatch`, no restart; second check: `skipped: image rejected before` |
| R5 | 2.1.4 | `o` | `REJECTED: bad signature` (optional, same check as R4) |
| R6 | 2.1.5 | `o` | `REJECTED: not signed` (optional) |
| R7 | 2.1.6 | `o`, then leave it alone (~15 min) | `TEST BUILD: never confirms`; 3x `not confirmed after 3 min online - restarting`; `rolling back`, `restored v2.1.2`; then `o`: `skipped: rolled back before` |
| R8 | 2.1.7 | `o`, then leave it alone (~2 min) | `TEST BUILD: hanging now`; watchdog reset after ~20 s, 3 times; `restored v2.1.2` |
