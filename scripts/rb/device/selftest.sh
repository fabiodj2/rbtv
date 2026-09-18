#!/bin/sh
# selftest.sh — consolidated PASS/FAIL smoke test for the rbtv deployment,
# run ON the Chromebit (as root). Unlike check-display.sh/check-audio.sh/
# usb-probe.sh (which dump raw diagnostics for a human to read), this script
# gives a single pass/fail verdict per subsystem plus one exit code, so it can
# be scripted from the workstation (see scripts/rb/run-device-tests.sh) and
# used to compare one deployment against another.
#
# REQUIRED checks fail the run (core player path). OPTIONAL checks only
# report (they depend on what is physically plugged in: DDJ-400, USB stick).
#
# Usage (on the device):
#   sh selftest.sh              # human-readable
#   sh selftest.sh -q           # summary line only
#
# Compare deployed artefacts against known-good hashes (see docs/16-handoff.md
# "Deployed / expected artefact hashes") by exporting any of:
#   EXPECT_RBP_MD5 EXPECT_FBDEV_MD5 EXPECT_MEMSHIM_MD5 EXPECT_AUDIOSHIM_MD5 EXPECT_KEYSHIM_MD5
#
# Exit: 0 if every REQUIRED check passes, 1 otherwise.
set -u

CH=/home/user/rbx3-run
QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

PASS_N=0
FAIL_N=0
SKIP_N=0

# result NAME STATUS DETAIL...
result() {
    name=$1; status=$2; shift 2
    case "$status" in
        PASS) PASS_N=$((PASS_N + 1)) ;;
        FAIL) FAIL_N=$((FAIL_N + 1)) ;;
        SKIP) SKIP_N=$((SKIP_N + 1)) ;;
    esac
    [ "$QUIET" = 1 ] && return 0
    printf '%-4s %-28s %s\n' "$status" "$name" "$*"
}

hash_check() {
    # hash_check LABEL FILE EXPECT_VAR
    label=$1; file=$2; var=$3
    eval "expect=\${$var:-}"
    if [ ! -f "$file" ]; then
        result "$label" FAIL "missing: $file"
        return
    fi
    actual=$(md5sum "$file" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$expect" ]; then
        result "$label" PASS "present, md5=$actual (no $var set to compare against)"
    elif [ "$actual" = "$expect" ]; then
        result "$label" PASS "md5=$actual matches $var"
    else
        result "$label" FAIL "md5=$actual != $var=$expect"
    fi
}

[ "$QUIET" = 1 ] || echo "== rbtv selftest =="

# ---- REQUIRED: rb.service ----
if systemctl is-active --quiet rb.service 2>/dev/null; then
    result "rb.service" PASS "active"
else
    result "rb.service" FAIL "$(systemctl is-active rb.service 2>&1)"
fi

# ---- REQUIRED: rbp process ----
if pgrep -f '/root/pdj/rbp -a' >/dev/null 2>&1; then
    result "rbp process" PASS "running (pid $(pgrep -f '/root/pdj/rbp -a' | head -1))"
else
    result "rbp process" FAIL "not running"
fi

# ---- REQUIRED: HDMI display ----
if [ -e /sys/class/graphics/fb0/name ]; then
    conn=$(cat /sys/class/drm/card0-HDMI-A-1/status 2>/dev/null || echo unknown)
    if [ "$conn" = "connected" ]; then
        result "HDMI display" PASS "fb0=$(cat /sys/class/graphics/fb0/name), connector=$conn"
    else
        result "HDMI display" FAIL "connector status=$conn"
    fi
else
    result "HDMI display" FAIL "no /sys/class/graphics/fb0"
fi

# ---- REQUIRED: ALSA HDMI PCM ----
if [ -e /proc/asound/card0/pcm0p/sub0/status ]; then
    result "ALSA HDMI PCM" PASS "card0/pcm0p present"
else
    result "ALSA HDMI PCM" FAIL "no /proc/asound/card0/pcm0p"
fi

# ---- REQUIRED: deployed artefacts present (+ optional hash compare) ----
hash_check "chroot rbp"         "$CH/root/pdj/rbp"                                             EXPECT_RBP_MD5
hash_check "directfb fbdev"     "$CH/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"       EXPECT_FBDEV_MD5
hash_check "memshim.so"         "$CH/usr/lib/memshim.so"                                        EXPECT_MEMSHIM_MD5
hash_check "audioshim.so"       "$CH/usr/lib/audioshim.so"                                       EXPECT_AUDIOSHIM_MD5
hash_check "keyshim.so"         "$CH/usr/lib/keyshim.so"                                         EXPECT_KEYSHIM_MD5

# ---- OPTIONAL: rbkeyd (physical keyboard) ----
if systemctl is-active --quiet rbkeyd.service 2>/dev/null; then
    result "rbkeyd.service" PASS "active"
else
    result "rbkeyd.service" SKIP "$(systemctl is-active rbkeyd.service 2>&1)"
fi

# ---- OPTIONAL: DDJ-400 bridge (only meaningful if plugged in) ----
if systemctl is-active --quiet ddj400-bridge.service 2>/dev/null; then
    result "ddj400-bridge.service" PASS "active"
else
    result "ddj400-bridge.service" SKIP "$(systemctl is-active ddj400-bridge.service 2>&1) (ok if DDJ-400 not plugged in)"
fi

# ---- OPTIONAL: USB1 rekordbox library ----
if [ -x /home/user/usb-probe.sh ]; then
    if /home/user/usb-probe.sh -q >/tmp/selftest-usb.log 2>&1; then
        result "USB1 library" PASS "detected ($(tail -1 /tmp/selftest-usb.log))"
    else
        result "USB1 library" SKIP "not detected (ok if no stick plugged in)"
    fi
else
    result "USB1 library" SKIP "usb-probe.sh not installed on device"
fi

[ "$QUIET" = 1 ] || echo
if [ "$FAIL_N" -eq 0 ]; then
    echo "selftest: PASS ($PASS_N passed, $SKIP_N skipped)"
    exit 0
else
    echo "selftest: FAIL ($FAIL_N failed, $PASS_N passed, $SKIP_N skipped)"
    exit 1
fi
