#!/bin/sh
# Builds the OTA test set for the REAL tracker firmware (stepwhere/) into
# prebuilt/ota_real_test/:
#   app_bins/stepwhere_v2.1.2.bin            good update (R1-R3), also packed as
#                                            2.1.3 corrupt / 2.1.4 dev key / 2.1.5 unsigned
#   app_bins/stepwhere_v2.1.6_noconfirm.bin  never confirms, restarts after 3 min online (R7)
#   app_bins/stepwhere_v2.1.7_hang.bin       hangs at its trial boot (R8)
#   releases/2.1.4/stepwhere.ota             signed with the old dev key -> must be rejected
#   releases/2.1.5/stepwhere.ota             not signed -> must be rejected
# The production-signed files are made on the key owner's Mac with
# sign_with_my_key.sh. See README.md.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLI="${ARDUINO_CLI:-arduino-cli}"
PY="${PYTHON:-python3}"
FQBN=STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX
OUT="$ROOT/prebuilt/ota_real_test"
TMP="${BUILD_DIR:-${TMPDIR:-/tmp}/ota_real_test_build}"
T="$ROOT/tools/ota_tool.py"
mkdir -p "$OUT/app_bins" "$TMP"

build() {   # out-name version extra-flags
    echo "== $1 ($2)"
    "$CLI" compile --fqbn "$FQBN" --libraries "$ROOT/libraries" --warnings default \
        --build-property build.flash_offset=0x4000 \
        --build-property "compiler.cpp.extra_flags=-DAPP_VERSION=\"$2\" $3" \
        --build-path "$TMP/$1" "$ROOT/stepwhere" | grep -E "Sketch uses|warning|error" || true
    cp "$TMP/$1/stepwhere.ino.bin" "$OUT/app_bins/$1.bin"
}

build stepwhere_v2.1.2           v2.1.2-lis2dh12 ""
build stepwhere_v2.1.6_noconfirm v2.1.6-lis2dh12 "-DOTA_TEST_NO_CONFIRM=1 -DOTA_TRIAL_ONLINE_LIMIT_MS=180000UL"
build stepwhere_v2.1.7_hang      v2.1.7-lis2dh12 "-DOTA_TEST_HANG=1"

mkdir -p "$OUT/releases/2.1.4" "$OUT/releases/2.1.5"
"$PY" "$T" pack --bin "$OUT/app_bins/stepwhere_v2.1.2.bin" --version 2.1.4 --git-sha 0000000000000000000000000000000000000000 \
    --key "$ROOT/tools/dev_keys/ota_signing_key.pem" --out "$OUT/releases/2.1.4/stepwhere.ota"
"$PY" "$T" pack --bin "$OUT/app_bins/stepwhere_v2.1.2.bin" --version 2.1.5 --git-sha 0000000000000000000000000000000000000000 \
    --out "$OUT/releases/2.1.5/stepwhere.ota"
ls -l "$OUT/app_bins" "$OUT/releases"/*
