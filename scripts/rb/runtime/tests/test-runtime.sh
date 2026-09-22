#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME=$(CDPATH= cd -- "$HERE/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
RX3_RUNTIME_ROOT="$RUNTIME" RX3_RUNTIME_LOG="$TMP/runtime.log" sh "$RUNTIME/runtime.sh"
grep -q 'loaded modules: core' "$TMP/runtime.log"
echo "PASS: modular runtime foundation"
