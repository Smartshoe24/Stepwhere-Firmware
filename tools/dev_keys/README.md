# DEVELOPMENT signing key: NOT SECRET, NOT FOR PRODUCTS

This key pair is committed on purpose so bench-test images can be signed by
anyone. A bootloader built with it prints
`[BOOT] WARNING: development signing key` at every boot.

Before shipping devices:

1. Run `python3 tools/ota_tool.py keygen --out keys/` on a trusted computer.
2. Store `keys/ota_signing_key.pem` as the GitHub secret `OTA_SIGNING_KEY`
   and keep an offline backup. Never commit it.
3. Copy `keys/ota_public_key.h` to `libraries/StepwhereOTA/src/ota_public_key.h`,
   rebuild the bootloader and the app.
