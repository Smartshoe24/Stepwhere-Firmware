#!/bin/sh
# Signs the real-firmware OTA test set with YOUR production key. Run on the
# key owner's Mac, from the Stepwhere-Firmware folder:
#   sh tests/ota_real_test/sign_with_my_key.sh ~/stepwhere-keys/ota_signing_key.pem
# Creates prebuilt/ota_real_test/releases/<version>/stepwhere.ota:
#   2.1.2  good update
#   2.1.3  signed, then one byte damaged -> must be rejected (SHA-256)
#   2.1.6  never confirms -> restarts itself, rolls back after 3 tries
#   2.1.7  hangs -> watchdog -> rolls back
# (2.1.4 dev key and 2.1.5 unsigned are already there.)
# Upload the whole "releases" folder afterwards (README.md).
set -e
KEY="${1:?usage: sh tests/ota_real_test/sign_with_my_key.sh /path/to/ota_signing_key.pem}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$ROOT/prebuilt/ota_real_test"
PY="${PYTHON:-python3}"
T="$ROOT/tools/ota_tool.py"
SHA="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo 0000000000000000000000000000000000000000)"

pack() {   # bin version
    mkdir -p "$D/releases/$2"
    "$PY" "$T" pack --bin "$D/app_bins/$1.bin" --version "$2" --key "$KEY" --git-sha "$SHA" \
        --out "$D/releases/$2/stepwhere.ota"
}
pack stepwhere_v2.1.2           2.1.2
pack stepwhere_v2.1.2           2.1.3
pack stepwhere_v2.1.6_noconfirm 2.1.6
pack stepwhere_v2.1.7_hang      2.1.7
"$PY" -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=0xFF; open(p,'wb').write(b)" \
    "$D/releases/2.1.3/stepwhere.ota"

echo
echo "Check against the production key in the bootloader (all must say OK):"
for v in 2.1.2 2.1.6 2.1.7; do
    printf "  %s: " "$v"
    "$PY" "$T" verify "$D/releases/$v/stepwhere.ota" --pubkey "$ROOT/tools/production_public_key.pem" || {
        echo "STOP: this is not the production key (fingerprint 59C07275). Nothing to upload."; exit 1; }
done
echo
echo "Done. Upload this folder to GitHub (see README.md):"
echo "  $D/releases"
