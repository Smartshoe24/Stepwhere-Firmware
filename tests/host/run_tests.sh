#!/bin/sh
# Builds and runs the host unit tests for the OTA code.
# Needs only a C compiler (gcc or clang) and Python 3 with "cryptography".
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/libraries/StepwhereOTA/src"
HOST="$ROOT/tests/host"
PY="${PYTHON:-python3}"

# Set SANITIZE=1 to build with AddressSanitizer/UBSan (needs libasan installed).
SAN=""
[ "${SANITIZE:-0}" = "1" ] && SAN="-fsanitize=address,undefined"
CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g $SAN -I$SRC -I$HOST -I$ROOT/bootloader/src"
LIBSRC="$SRC/w25q.c $SRC/ota_crc32.c $SRC/ota_sha256.c $SRC/ota_image.c $SRC/ota_meta.c"

echo "== W25Q128 driver"
${CC:-cc} $CFLAGS "$HOST/test_w25q.c" "$HOST/fake_w25q.c" $LIBSRC -o "$HOST/test_w25q.out"
"$HOST/test_w25q.out"

echo "== Bootloader state machine"
# Image made by the Python tool, to check the C code reads the same format.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$PY" - "$ROOT/tools" "$TMP/fw.ota" <<'EOF'
import struct, sys
sys.path.insert(0, sys.argv[1])
import ota_tool as t
app = struct.pack("<II", 0x20009000, 0x08004101) + bytes((i * 13 + 5) & 0xFF for i in range(70000))
open(sys.argv[2], "wb").write(t.build_image(app, t.parse_version("1.3.0"), t.OTA_HW_ID_GSM_PCB_R1))
EOF
${CC:-cc} $CFLAGS "$HOST/test_boot.c" "$HOST/fake_w25q.c" "$ROOT/bootloader/src/boot_core.c" $LIBSRC \
    -o "$HOST/test_boot.out"
"$HOST/test_boot.out" "$TMP/fw.ota"

echo "== Packaging tool"
"$PY" "$HOST/test_ota_tool.py"
