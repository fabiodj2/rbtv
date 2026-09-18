#!/bin/sh
# build-dfbtest.sh — build the DirectFB bring-up probe (soft-float, glibc 2.13)
# against the RX3 runtime sysroot.  It isolates the display stack from rbp:
# DirectFBInit -> CreateSurface(primary) -> Clear -> Flip.
#
# Usage:
#   RX3=/path/to/XDJRX3-rootfs ./scripts/rb/probe/build-dfbtest.sh
#
# Deploy the result to the chroot as /root/dfbtest and run it with the shims:
#   chroot /home/user/rbx3-run env DFB_ROTATE=off \
#       LD_PRELOAD=/usr/lib/memshim.so:/usr/lib/fbshim.so \
#       /lib/ld-linux.so.3 /root/dfbtest
set -eu

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
RBX3=${RBX3:-$REPO/../rbx3-firmware}
RX3=${RX3:-$RBX3/XDJRX3-rootfs}
CROSS=${CROSS:-arm-linux-gnueabi-}
OUT=${OUT:-$REPO/work/rb/probe/dfbtest}

[ -d "$RX3" ] || { echo "RX3 rootfs not found: $RX3" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"

# DirectFB headers come from the soft-float sysroot assembled by
# build-directfb.sh (/tmp/arm213sysroot); fall back to the RX3 include dir.
SYS=${SYS:-/tmp/arm213sysroot}
INC="$SYS/usr/include"
[ -f "$INC/directfb.h" ] || INC="$RX3/usr/include"

echo "== build dfbtest (soft-float) =="
${CROSS}gcc -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector \
    -I"$INC" -o "$OUT" "$REPO/scripts/rb/probe/dfbtest.c" \
    -L"$RX3/lib" -L"$RX3/usr/lib" \
    -ldirectfb -ldirect -lfusion -ldl -lpthread \
    -Wl,-rpath-link,"$RX3/lib:$RX3/usr/lib"

echo "== GLIBC versions =="
${CROSS}objdump -T "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -u
md5sum "$OUT"
