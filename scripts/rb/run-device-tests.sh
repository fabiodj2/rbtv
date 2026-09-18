#!/bin/sh
# run-device-tests.sh — from the workstation, push selftest.sh to a running
# Chromebit and run it over SSH, so the pass/fail smoke test (see
# scripts/rb/device/selftest.sh) can be scripted right after a deploy instead
# of eyeballing check-display.sh/check-audio.sh/usb-probe.sh output by hand.
#
# Usage:
#   PASS=<password> HOST=root@chromebit.local ./scripts/rb/run-device-tests.sh
#   PASS=<password> ./scripts/rb/run-device-tests.sh -q     # summary line only
#
# Compare against known-good artefact hashes (docs/16-handoff.md):
#   PASS=<password> EXPECT_RBP_MD5=18a64bc4d0ffd1cbd35f3a6ea447fca8 \
#       ./scripts/rb/run-device-tests.sh
#
# Exit code is selftest.sh's: 0 if every REQUIRED check passed.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=${HOST:-root@chromebit.local}
PASS=${PASS:?set PASS to the device password}
SELFTEST=$REPO/scripts/rb/device/selftest.sh
S="sshpass -p $PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $HOST"

[ -f "$SELFTEST" ] || { echo "missing $SELFTEST" >&2; exit 1; }

$S "cat > /tmp/rbtv-selftest.sh" < "$SELFTEST"

# Forward EXPECT_*_MD5 (if set) and -q through to the remote run.
ENVVARS=""
for v in EXPECT_RBP_MD5 EXPECT_FBDEV_MD5 EXPECT_MEMSHIM_MD5 EXPECT_AUDIOSHIM_MD5 EXPECT_KEYSHIM_MD5; do
    eval "val=\${$v:-}"
    [ -n "$val" ] && ENVVARS="$ENVVARS $v='$val'"
done

$S "env$ENVVARS sh /tmp/rbtv-selftest.sh ${1:-}"
