#!/bin/sh
# build-rk3399-shims.sh — rebuild the ARM32 LD_PRELOAD shims for the RK3399
# runtime from THIS repo's sources, using the runtime chroot as the sysroot.
#
# Why this exists: build-shims.sh needs a PrimeBox checkout and an extracted RX3
# rootfs, so on the Orange Pi nothing rebuilt work/build/arm32/shims after the
# source changed. memshim.so ran for weeks from a 2026-09-18 build that predated
# the RX3_TOUCH_BRIDGE read path in memshim.c, and the touchscreen never worked
# (docs/21-rbpi4b-comparison.md). scripts/rb/device/doctor-rk3399.sh now flags
# a deployed shim older than its source.
#
# Usage:
#   scripts/rb/build-rk3399-shims.sh                  # memshim.so
#   scripts/rb/build-rk3399-shims.sh memshim.so keyshim.so
# Output: work/build/arm32/shims/<name>.so (deploying is a separate step).
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RUNTIME=${RUNTIME:-$REPO/work/runtime/rx3}
OUT=${OUT:-$REPO/work/build/arm32/shims}
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

[ -d "$RUNTIME/lib" ] || { echo "runtime sysroot not found: $RUNTIME" >&2; exit 1; }
[ -f "$OUT/Makefile" ] || { echo "missing $OUT/Makefile" >&2; exit 1; }

# The runtime has no libc_nonshared.a / libpthread_nonshared.a.
mkdir -p "$STAGE/compat"
( cd "$STAGE/compat" && ar rcs libc_nonshared.a && ar rcs libpthread_nonshared.a )

cp "$OUT/Makefile" "$STAGE/"
for src in memshim.c keyshim.c audioshim.c; do
    cp "$REPO/scripts/rb/$src" "$STAGE/"
done

TARGETS=${*:-memshim.so}
make -C "$STAGE" RX3="$RUNTIME" \
    LDFLAGS="-L$STAGE/compat -L$RUNTIME/lib -L$RUNTIME/usr/lib -Wl,-rpath-link,$RUNTIME/lib:$RUNTIME/usr/lib" \
    $TARGETS

for t in $TARGETS; do
    vers=$(arm-linux-gnueabi-objdump -T "$STAGE/$t" | grep -o 'GLIBC_[0-9.]*' | sort -u | tr '\n' ' ')
    case "$vers" in
        *GLIBC_2.[1-9][0-9]*) echo "FAIL: $t needs $vers (chroot is glibc 2.13)" >&2; exit 1 ;;
    esac
    install -m 0755 "$STAGE/$t" "$OUT/$t"
    printf '%-14s %s  %s\n' "$t" "$(md5sum < "$OUT/$t" | cut -c1-32)" "$vers"
done
echo "deploy: cp $OUT/<name>.so $RUNTIME/usr/lib/ (back up first), then restart"
