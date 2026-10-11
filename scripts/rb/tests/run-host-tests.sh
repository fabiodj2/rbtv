#!/bin/sh
# run-host-tests.sh - x86-64 host tests for the shim safety fixes (docs/18).
# No ARM toolchain, board or rbp needed:
#   * audioshim against a fake libasound (partial writes, xrun, dead sink,
#     close-vs-writer race, log cap/rotation, sample-format dump)
#   * keyshim's UiMain pump guard with a mocked PanelComPeerLinux (K2)
#   * keyshim K1/K11: inert on md5 mismatch, kill-switch, non-player process
# Usage: sh scripts/rb/tests/run-host-tests.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
RB=$(dirname "$HERE")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CC=${CC:-gcc}
fail=0

echo "== audioshim (fake libasound) =="
$CC -O2 -fPIC -shared -o "$W/libasound.so.2" "$HERE/fakealsa.c"
# small log cap so rotation is exercised
$CC -O2 -fPIC -shared -Wall -Wextra -Wno-unused-parameter -DLOG_CAP_BYTES=8192 \
    -o "$W/audioshim.so" "$RB/audioshim.c" -lpthread -ldl
$CC -O2 -o "$W/audiotest" "$HERE/audiotest.c" "$W/audioshim.so" -lpthread -ldl -Wl,-rpath,"$W"
rm -f /tmp/audioshim.log /tmp/audioshim.log.1
STARTUP_MUTE_MS=0 AUDIOSHIM_DUMP=1 LD_LIBRARY_PATH="$W" "$W/audiotest" || fail=1

echo "== keyshim pump guard (K2) =="
$CC -O2 -Wall -Wno-int-to-pointer-cast -Wno-unused-function -I"$RB" \
    -o "$W/pumptest" "$HERE/pumptest.c" -lpthread
"$W/pumptest" || fail=1

echo "== keyshim identity / kill-switch (K1, K11) =="
$CC -O2 -fPIC -shared -Wno-int-to-pointer-cast -o "$W/keyshim.so" "$RB/keyshim.c" -lpthread
cp /bin/sleep "$W/rbp"; cp /bin/sleep "$W/other"
rm -f /tmp/keyshim.log
LD_PRELOAD="$W/keyshim.so" "$W/rbp" 1
grep -q "REFUSING to run" /tmp/keyshim.log && echo "ok:   md5 mismatch -> inert" || { echo "FAIL: md5 mismatch"; fail=1; }
rm -f /tmp/keyshim.log
KEYSHIM_DISABLE=1 LD_PRELOAD="$W/keyshim.so" "$W/rbp" 1
[ ! -e /tmp/keyshim.log ] && echo "ok:   KEYSHIM_DISABLE=1 -> no threads, no log" || { echo "FAIL: kill-switch"; fail=1; }
LD_PRELOAD="$W/keyshim.so" "$W/other" 1
[ ! -e /tmp/keyshim.log ] && echo "ok:   non-player process untouched" || { echo "FAIL: non-player"; fail=1; }
rm -f /tmp/keyshim.log

[ "$fail" = 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
