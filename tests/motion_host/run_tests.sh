#!/bin/sh
# Builds and runs the host tests for the LIS2DH12 driver and the step counter,
# and builds the step_replay tool.
# Needs only a C compiler (gcc or clang).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/libraries/StepwhereMotion/src"
HOST="$ROOT/tests/motion_host"
OUT="${OUT_DIR:-$HOST/build}"
mkdir -p "$OUT"

# Set SANITIZE=1 to build with AddressSanitizer/UBSan (needs libasan installed).
SAN=""
[ "${SANITIZE:-0}" = "1" ] && SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion -O1 -g $SAN -I$SRC -I$HOST"

echo "== LIS2DH12 driver"
${CC:-cc} $CFLAGS "$HOST/test_lis2dh12.c" "$HOST/fake_lis2dh12.c" "$SRC/lis2dh12.c" -o "$OUT/test_lis2dh12"
"$OUT/test_lis2dh12"

echo "== Motion log in external flash"
OTA="$ROOT/libraries/StepwhereOTA/src"
${CC:-cc} $CFLAGS -Wno-conversion -I"$OTA" -I"$ROOT/tests/host" "$HOST/test_motion_log.c" "$SRC/motion_log.c" \
    "$OTA/w25q.c" "$ROOT/tests/host/fake_w25q.c" -o "$OUT/test_motion_log"
"$OUT/test_motion_log"

echo "== Step counter"
${CC:-cc} $CFLAGS -Wno-conversion "$HOST/test_step_counter.c" "$HOST/motion_sim.c" "$SRC/step_counter.c" \
    -lm -o "$OUT/test_step_counter"
"$OUT/test_step_counter"

echo "== Tools"
${CC:-cc} $CFLAGS "$HOST/step_replay.c" "$SRC/step_counter.c" -o "$OUT/step_replay"
${CC:-cc} $CFLAGS -Wno-conversion "$HOST/sim_dump.c" "$HOST/motion_sim.c" -lm -o "$OUT/sim_dump"
# Smoke test: the replay tool must agree with the unit test on a synthetic walk.
"$OUT/sim_dump" walk 50 > "$OUT/walk50.csv"
"$OUT/step_replay" --decimate 2 "$OUT/walk50.csv" | tail -1 | grep -q "TOTAL 100 steps" \
    || { echo "step_replay smoke test FAILED"; exit 1; }
echo "built $OUT/step_replay and $OUT/sim_dump (smoke test passed)"

echo "== Real recordings"
# Expected step ranges for recordings with a known result (recordings/*/README.md).
check_rec() {   # file min max
    n=$("$OUT/step_replay" --decimate 2 "$ROOT/$1" | tail -1 | awk '{print $2}')
    if [ "$n" -ge "$2" ] && [ "$n" -le "$3" ]; then echo "  ok   $1: $n steps ($2..$3)"
    else echo "  FAIL $1: $n steps, expected $2..$3"; exit 1; fi
}
check_rec recordings/2026-10-31/walk1_s1_still.csv 0 0
check_rec recordings/2026-10-31/walk1_s2_walk.csv 110 124
