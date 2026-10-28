#!/bin/sh
# Builds and runs the host unit tests for the OTA library code.
# Needs only a C compiler (gcc or clang) and Python 3 with "cryptography".
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/libraries/StepwhereOTA/src"
OUT="$ROOT/tests/host/test_w25q.out"

# Set SANITIZE=1 to build with AddressSanitizer/UBSan (needs libasan installed).
SAN=""
[ "${SANITIZE:-0}" = "1" ] && SAN="-fsanitize=address,undefined"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g $SAN \
    -I"$SRC" "$ROOT/tests/host/test_w25q.c" "$SRC/w25q.c" -o "$OUT"
"$OUT"

${PYTHON:-python3} "$ROOT/tests/host/test_ota_tool.py"
