#!/bin/sh
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$REPO/work/build/host}

mkdir -p "$OUT"

echo "== ddj400-bridge"
gcc -O2 -Wall -Wextra \
    -o "$OUT/ddj400-bridge" \
    "$REPO/scripts/rb/device/ddj400-bridge.c"

echo "== rx3-touch-bridge"
gcc -O2 -Wall -Wextra \
    -I"$REPO/scripts/rb/device" \
    -o "$OUT/rx3-touch-bridge" \
    "$REPO/scripts/rb/device/rx3-touch-bridge.c"

file "$OUT/ddj400-bridge" "$OUT/rx3-touch-bridge"
echo "PASS: helpers nativos em $OUT"
