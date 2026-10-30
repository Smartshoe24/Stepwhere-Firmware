#!/bin/sh
# Builds everything into dist/ (used by GitHub Actions, also works locally):
#   dist/bootloader.bin/.hex                 bootloader (0x08000000)
#   dist/stepwhere_app.bin                   main firmware linked at 0x08004000
#   dist/stepwhere_factory.hex               bootloader + firmware, for STM32CubeProgrammer
#   dist/flash_selftest.bin, ota_test_app.bin, modem_probe.bin   bench tools
#
# Needs: arduino-cli with STMicroelectronics:stm32 2.x and the libraries listed
# in README.md, Python 3. CROSS defaults to the ARM gcc shipped with the core.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ARDUINO_CLI:-arduino-cli}"
PY="${PYTHON:-python3}"
FQBN=STMicroelectronics:stm32:GenG0:pnum=GENERIC_G070CBTX
OUT="$ROOT/dist"
BUILD="${BUILD_DIR:-$ROOT/build}"
mkdir -p "$OUT" "$BUILD"

if [ -z "$CROSS" ]; then
    DATA="$("$CLI" config get directories.data 2>/dev/null || echo "$HOME/.arduino15")"
    GCC_BIN="$(ls -d "$DATA"/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/*/bin 2>/dev/null | tail -1)"
    [ -n "$GCC_BIN" ] || { echo "arm-none-eabi-gcc not found; set CROSS"; exit 1; }
    CROSS="$GCC_BIN/arm-none-eabi-"
fi

echo "== bootloader"
make -s -C "$ROOT/bootloader" CROSS="$CROSS"
cp "$ROOT/bootloader/build/bootloader.bin" "$ROOT/bootloader/build/bootloader.hex" "$OUT/"

compile() {   # sketch-dir out-name [extra args...]
    dir="$1"; name="$2"; shift 2
    echo "== $name"
    "$CLI" compile --fqbn "$FQBN" --libraries "$ROOT/libraries" --warnings default \
        --build-path "$BUILD/$name" "$@" "$dir" | grep -E "Sketch uses|Global variables"
    cp "$BUILD/$name/$(basename "$dir").ino.bin" "$OUT/$name.bin"
}

compile "$ROOT/stepwhere" stepwhere_app --build-property build.flash_offset=0x4000
compile "$ROOT/tests/ota_test_app" ota_test_app --build-property build.flash_offset=0x4000
compile "$ROOT/tests/flash_selftest" flash_selftest
compile "$ROOT/tests/modem_probe" modem_probe

echo "== factory image"
"$PY" "$ROOT/tools/ota_tool.py" merge --bootloader "$OUT/bootloader.bin" \
    --app "$OUT/stepwhere_app.bin" --out "$OUT/stepwhere_factory.hex"
ls -l "$OUT"
