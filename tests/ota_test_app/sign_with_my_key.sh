#!/bin/sh
# Signs the bench-test apps with YOUR production key. Run on the key owner's Mac:
#   sh tests/ota_test_app/sign_with_my_key.sh ~/stepwhere-keys/ota_signing_key.pem
# Creates in prebuilt/ota_test/:
#   testapp_v1.1.ota            good update
#   testapp_v1.2_noconfirm.ota  never confirms -> rollback
#   testapp_v1.3_corrupt.ota    signed, then one byte damaged -> rejected (SHA-256)
#   testapp_v1.4_hang.ota       hangs -> watchdog -> rollback
set -e
KEY="${1:?usage: sh tests/ota_test_app/sign_with_my_key.sh /path/to/ota_signing_key.pem}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$ROOT/prebuilt/ota_test"
PY="${PYTHON:-python3}"
T="$ROOT/tools/ota_tool.py"
"$PY" "$T" pack --bin "$D/app_bins/testapp_v1.1.bin"           --version 1.1.0 --key "$KEY" --out "$D/testapp_v1.1.ota"
"$PY" "$T" pack --bin "$D/app_bins/testapp_v1.2_noconfirm.bin" --version 1.2.0 --key "$KEY" --out "$D/testapp_v1.2_noconfirm.ota"
"$PY" "$T" pack --bin "$D/app_bins/testapp_v1.3.bin"           --version 1.3.0 --key "$KEY" --out "$D/testapp_v1.3_corrupt.ota"
"$PY" "$T" pack --bin "$D/app_bins/testapp_v1.4_hang.bin"      --version 1.4.0 --key "$KEY" --out "$D/testapp_v1.4_hang.ota"
"$PY" -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=0xFF; open(p,'wb').write(b)" "$D/testapp_v1.3_corrupt.ota"
echo
echo "Check against the key compiled into the bootloader:"
for f in testapp_v1.1 testapp_v1.2_noconfirm testapp_v1.4_hang; do
    printf "  %s: " "$f"; "$PY" "$T" verify "$D/$f.ota" --pubkey "$ROOT/tools/production_public_key.pem"
done
