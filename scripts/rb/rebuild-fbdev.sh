#!/bin/sh
# rebuild-fbdev.sh — fast path: rebuild ONLY the Chromebit DirectFB fbdev module
# from an already-configured DirectFB 1.4.16 build tree, fix the sonames, verify
# the GLIBC symbol versions and stage the result into work/rb/dfb.
#
# Use this while iterating on the fbdev driver.  Use build-directfb.sh for the
# first full build (clone + patch + configure + make of the whole DirectFB core).
#
# Usage:
#   BUILD=/tmp/dfb OUT=$PWD/work/rb/dfb ./scripts/rb/rebuild-fbdev.sh
#
# The build tree must already have BOTH patches applied:
#   PrimeBox tools/build-directfb/directfb-full.diff
#   scripts/rb/directfb-chromebit.patch
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-/tmp/dfb}
OUT=${OUT:-$REPO/work/rb/dfb}
CROSS=${CROSS:-arm-linux-gnueabi-}

[ -d "$BUILD/systems/fbdev" ] || {
    echo "no DirectFB build tree at $BUILD — run scripts/rb/build-directfb.sh first" >&2
    exit 1
}

echo "== rebuild systems/fbdev =="
cd "$BUILD"

# The module is compiled with NEON (softfp keeps the soft-float *calling*
# convention, so it stays ABI-compatible with the soft-float DirectFB/rbp).
# This speeds up the RGB565->RGB32 + scale publish path ~2.6x (docs/07 F7).
# Set NEON=0 to build the portable scalar version.
NEON=${NEON:-1}
FBDEV_CFLAGS=""
if [ "$NEON" = "1" ]; then
    FBDEV_CFLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=softfp"
    echo "    (NEON: $FBDEV_CFLAGS)"
fi
BASECFLAGS=$(sed -n 's/^CFLAGS = //p' systems/fbdev/Makefile)

# DirectFB 1.4's dependency tracking is broken: always delete the objects first
# or edits to fbdev.c are silently ignored.
rm -f systems/fbdev/fbdev.lo systems/fbdev/.libs/fbdev.o \
      systems/fbdev/.libs/libdirectfb_fbdev.so
make -C systems/fbdev CFLAGS="$BASECFLAGS $FBDEV_CFLAGS" libdirectfb_fbdev.la

SO=systems/fbdev/.libs/libdirectfb_fbdev.so

echo "== fix NEEDED sonames (.so.6 -> .so.0, matching the RX3 libs) =="
for s in libdirect-1.4.so.6 libfusion-1.4.so.6 libdirectfb-1.4.so.6; do
    patchelf --replace-needed "$s" "${s%.6}.0" "$SO" 2>/dev/null || true
done
${CROSS}objdump -p "$SO" | grep NEEDED

echo "== GLIBC symbol versions (must be only 2.4 / 2.7) =="
${CROSS}objdump -T "$SO" | grep -o 'GLIBC_[0-9.]*' | sort -u

mkdir -p "$OUT/lib/directfb-1.4-6/systems"
cp "$SO" "$OUT/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"
echo "staged: $OUT/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"
md5sum "$OUT/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so"
