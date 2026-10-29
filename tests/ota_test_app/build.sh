#!/bin/sh
# Builds the OTA bench-test set into prebuilt/ota_test/:
#   bootloader_testapp_v1.0.bin/.hex  bootloader + test app v1.0, flash at 0x08000000
#   testapp_v1.1.ota                  good update
#   testapp_v1.2_noconfirm.ota        update that never confirms -> rollback
#   testapp_v1.3_corrupt.ota          damaged file -> must be rejected
#   testapp_v1.4_hang.ota             hangs -> watchdog reset -> rollback
#   testapp_v1.5_unsigned.ota         no signature -> must be rejected
#   testapp_v1.6_wrongkey.ota         signed with another key -> must be rejected
# All good files are signed with the DEVELOPMENT key (tools/dev_keys).
#
# Needs arduino-cli with the STM32 core 2.x, arm-none-eabi-gcc (for the
# bootloader) and Python 3. Set ARDUINO_CLI and CROSS if not on PATH.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CLI="${ARDUINO_CLI:-arduino-cli}"
PY="${PYTHON:-python3}"
FQBN=STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX
OUT="$ROOT/prebuilt/ota_test"
TMP="${TMPDIR:-/tmp}/ota_test_build"
mkdir -p "$OUT" "$TMP"

make -s -C "$ROOT/bootloader" ${CROSS:+CROSS=$CROSS}

build() {   # name major minor patch noconfirm hang
    "$CLI" compile --fqbn "$FQBN" --libraries "$ROOT/libraries" \
        --build-property build.flash_offset=0x4000 \
        --build-property "compiler.cpp.extra_flags=-DTEST_APP_VERSION_MAJOR=$2 -DTEST_APP_VERSION_MINOR=$3 -DTEST_APP_VERSION_PATCH=$4 -DTEST_APP_NO_CONFIRM=$5 -DTEST_APP_HANG=${6:-0}" \
        --build-path "$TMP/$1" "$ROOT/tests/ota_test_app" | grep -E "Sketch uses|error" || true
}

build v1.0 1 0 0 0
build v1.1 1 1 0 0
build v1.2 1 2 0 1
build v1.3 1 3 0 0
build v1.4 1 4 0 0 1
build v1.5 1 5 0 0
build v1.6 1 6 0 0

BIN=ota_test_app.ino.bin
"$PY" "$ROOT/tools/ota_tool.py" merge --bootloader "$ROOT/bootloader/build/bootloader.bin" \
    --app "$TMP/v1.0/$BIN" --out "$OUT/bootloader_testapp_v1.0.hex"
"$PY" - "$OUT/bootloader_testapp_v1.0.hex" "$OUT/bootloader_testapp_v1.0.bin" <<'EOF'
import sys
mem, upper = {}, 0
for line in open(sys.argv[1]):
    raw = bytes.fromhex(line.strip()[1:])
    n, addr, rtype, data = raw[0], (raw[1] << 8) | raw[2], raw[3], raw[4:4 + raw[0]]
    if rtype == 4: upper = int.from_bytes(data, "big") << 16
    elif rtype == 0:
        for i, b in enumerate(data): mem[upper + addr + i] = b
lo, hi = min(mem), max(mem) + 1
open(sys.argv[2], "wb").write(bytes(mem.get(a, 0xFF) for a in range(lo, hi)))
EOF
KEY="$ROOT/tools/dev_keys/ota_signing_key.pem"
rm -f "$OUT"/*.ota
"$PY" "$ROOT/tools/ota_tool.py" pack --bin "$TMP/v1.1/$BIN" --version 1.1.0 --key "$KEY" --out "$OUT/testapp_v1.1.ota"
"$PY" "$ROOT/tools/ota_tool.py" pack --bin "$TMP/v1.2/$BIN" --version 1.2.0 --key "$KEY" --out "$OUT/testapp_v1.2_noconfirm.ota"
"$PY" "$ROOT/tools/ota_tool.py" pack --bin "$TMP/v1.3/$BIN" --version 1.3.0 --key "$KEY" --out "$OUT/testapp_v1.3_corrupt.ota"
"$PY" "$ROOT/tools/ota_tool.py" pack --bin "$TMP/v1.4/$BIN" --version 1.4.0 --key "$KEY" --out "$OUT/testapp_v1.4_hang.ota"
"$PY" "$ROOT/tools/ota_tool.py" pack --bin "$TMP/v1.5/$BIN" --version 1.5.0 --out "$OUT/testapp_v1.5_unsigned.ota"
rm -rf "$TMP/otherkey"
"$PY" "$ROOT/tools/ota_tool.py" keygen --out "$TMP/otherkey" > /dev/null
"$PY" "$ROOT/tools/ota_tool.py" pack --bin "$TMP/v1.6/$BIN" --version 1.6.0 --key "$TMP/otherkey/ota_signing_key.pem" \
    --out "$OUT/testapp_v1.6_wrongkey.ota"
# Flip one byte in the middle of the payload: SHA-256 check must fail.
"$PY" -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[len(b)//2]^=0xFF; open(p,'wb').write(b)" \
    "$OUT/testapp_v1.3_corrupt.ota"
ls -l "$OUT"
