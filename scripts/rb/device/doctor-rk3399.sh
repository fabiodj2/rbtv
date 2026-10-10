#!/bin/sh
# doctor-rk3399.sh — check the running RK3399 runtime and report. Changes
# NOTHING: no write, no mount, no restart.
#
# Adapted from github.com/fabiodj2/rbpi4b scripts/device/doctor.sh, whose
# headline check is the one that bit this tree: the shim rbp actually has
# mapped, read from /proc/<pid>/maps, against the build of the current source.
# memshim.so ran from a build older than memshim.c for weeks and the touch
# FIFO was never read (docs/21-rbpi4b-comparison.md).
#
# Usage: sudo scripts/rb/device/doctor-rk3399.sh      (exit 1 if anything FAILs)
set -u

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
RUNTIME=${RX3_RUNTIME:-$REPO/work/runtime/rx3}
FAILS=0
pass() { echo "PASS  $*"; }
warn() { echo "WARN  $*"; }
fail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); }

RBP=$(pgrep -f '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' | head -1)
if [ -n "$RBP" ]; then pass "rbp running, pid $RBP"; else warn "rbp not running (runtime checks skipped)"; fi

# --- shims: loaded copy vs the build of the current source -----------------
# name | source | build dirs searched for an artifact with the deployed md5
check_shim() {
    name=$1 src=$2; shift 2
    dep="$RUNTIME/usr/lib/$name"
    [ -f "$dep" ] || { fail "$name: not deployed at $dep"; return; }
    if [ -n "$RBP" ] && ! grep -q "/usr/lib/$name\$" "/proc/$RBP/maps" 2>/dev/null; then
        fail "$name: deployed but not mapped by rbp $RBP (LD_PRELOAD?)"
    fi
    sum=$(md5sum < "$dep" | cut -c1-32)
    built=
    for d in "$@"; do
        for f in "$d"/*.so; do
            [ -f "$f" ] && [ "$(md5sum < "$f" | cut -c1-32)" = "$sum" ] && { built=$f; break 2; }
        done
    done
    if [ -z "$built" ]; then
        warn "$name: deployed md5 ${sum%${sum#????????}} matches no build artifact (built elsewhere?)"
    elif [ "$REPO/$src" -nt "$built" ]; then
        # Only a hint: a `cp -p` keeps an old mtime on a fresh build (seen on
        # fbshim32.so). The content checks below are what FAIL.
        warn "$name: matching build ${built#$REPO/} ($(date -r "$built" '+%F %R')) is older than $src ($(date -r "$REPO/$src" '+%F %R')) — check, or rebuild with scripts/rb/build-rk3399-shims.sh"
    else
        pass "$name: matches ${built#$REPO/}, not older than $src"
    fi
}
SH="$REPO/work/build/arm32/shims $REPO/work/rb/shims"
check_shim memshim.so   scripts/rb/memshim.c   $SH
check_shim keyshim.so   scripts/rb/keyshim.c   $SH
check_shim audioshim.so scripts/rb/audioshim.c $SH
check_shim netshim.so   scripts/rb/netshim.c   $SH
check_shim fbshim.so    scripts/display/rk3399/fbshim32.c "$REPO/work/build/arm32/display"

if grep -q RX3_TOUCH_BRIDGE "$RUNTIME/usr/lib/memshim.so" 2>/dev/null; then
    pass "memshim.so has the RX3_TOUCH_BRIDGE read path"
else
    fail "memshim.so lacks RX3_TOUCH_BRIDGE: rbp gets zero touch records forever"
fi

# --- touch -------------------------------------------------------------------
IDENT=$(printf '0\n0\n320\n200\n1280\n800')
for c in TouchCalib_User.dat TouchCalib_Factory.dat; do
    if [ "$(cat "$RUNTIME/root/settings/$c" 2>/dev/null)" = "$IDENT" ]; then
        pass "$c is identity"
    else
        fail "$c missing or not identity (the launcher installs it)"
    fi
done

TSC="$RUNTIME/dev/tsc2007_2-0048"
if [ -p "$TSC" ] && [ -n "$RBP" ]; then
    backlog=$(python3 -c "
import os,fcntl,termios,struct
fd=os.open('$TSC',os.O_RDONLY|os.O_NONBLOCK)
print(struct.unpack('i',fcntl.ioctl(fd,termios.FIONREAD,b'\0'*4))[0])" 2>/dev/null || echo '?')
    case "$backlog" in
        '?') warn "touch FIFO backlog unreadable (run as root)" ;;
        0|6|12|18|24) pass "touch FIFO drained (backlog $backlog bytes)" ;;
        *) fail "touch FIFO holds $backlog unread bytes: rbp is not reading touch" ;;
    esac
fi

# --- rbp patch words (read from the binary on disk) --------------------------
python3 - "$RUNTIME/root/pdj/rbp" <<'EOF' || FAILS=$((FAILS + 1))
import struct, sys
d = open(sys.argv[1], 'rb').read()
w = lambda va: struct.unpack_from('<I', d, va - 0x8000)[0]
ok = True
v = w(0x3c665c)
if v == 0xea000054:
    print("PASS  scanForDevices @0x3c665c patched (RX3 device list)")
else:
    print("FAIL  scanForDevices @0x3c665c = %#x (expected 0xea000054)" % v); ok = False
if (w(0x31df64), w(0x31df68)) == (0xe3a00000, 0xe12fff1e):
    print("WARN  getPcController @0x31df64 is 'mov r0,#0; bx lr': no crash, but Pro DJ Link / USB-B PC link disabled (rbpi4b patch-rbp-nopc.py keeps them)")
sys.exit(0 if ok else 1)
EOF

# --- display -------------------------------------------------------------------
fb=/sys/class/graphics/fb0
[ -r $fb/virtual_size ] && echo "INFO  fb0 $(cat $fb/name) virtual=$(cat $fb/virtual_size) bpp=$(cat $fb/bits_per_pixel) stride=$(cat $fb/stride)"
for c in /sys/class/drm/card*-*/status; do
    [ -r "$c" ] && echo "INFO  $(basename "$(dirname "$c")"): $(cat "$c")"
done

echo
if [ $FAILS -eq 0 ]; then echo "doctor: all checks passed"; else echo "doctor: $FAILS check(s) FAILED"; fi
[ $FAILS -eq 0 ]
