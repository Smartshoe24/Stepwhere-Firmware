# OTA bench test files (production key)

The bootloader in `bootloader_testapp_v1.0.bin` trusts only the **production key**
(fingerprint `59C07275`, see `tools/production_public_key.pem`). It prints
`[BOOT] signing key 59C07275` at boot.

| File | Use |
|---|---|
| `bootloader_testapp_v1.0.bin` | Flash at **0x08000000** (bootloader v0.2.1 + test app v1.0) |
| `app_bins/*.bin` | Unsigned test apps. Sign them yourself (below). |
| `testapp_v1.5_devkey.ota` | Signed with the old development key. Must be rejected (bad signature). |
| `testapp_v1.6_unsigned.ota` | No signature. Must be rejected. |

Sign on the key owner's Mac:

```sh
sh tests/ota_test_app/sign_with_my_key.sh ~/stepwhere-keys/ota_signing_key.pem
```

This creates `testapp_v1.1.ota` (good), `testapp_v1.2_noconfirm.ota` (rollback),
`testapp_v1.3_corrupt.ota` (rejected) and `testapp_v1.4_hang.ota` (watchdog).

Upload: `python3 tools/ota_uart.py upload --port /dev/cu.usbserial-XXXX FILE --reset`.
Test app command `f` marks the download slot PENDING without checks.
