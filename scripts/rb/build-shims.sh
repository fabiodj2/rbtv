#!/bin/sh
# build-shims.sh — build the Chromebit LD_PRELOAD shims (soft-float, glibc 2.13)
# from PrimeBox's shim sources plus our Chromebit overrides:
#
#   work/rb/shims/memshim.so    <- scripts/rb/memshim.c      (Chromebit /dev/mem+mmap)
#   work/rb/shims/audioshim.so  <- scripts/rb/audioshim.c    (RX3 CS4344 -> HDMI hw:0,0)
#
# The other shims (fbshim-tsc, gpioshim, tscshim, crashcatch, knobshim2, ...)
# come from PrimeBox unchanged and are built too.
#
# Usage:
#   RX3=/path/to/XDJRX3-rootfs ./scripts/rb/build-shims.sh
#   ./scripts/rb/build-shims.sh memshim.so audioshim.so      # just these
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
PRIMEBOX=${PRIMEBOX:-$REPO/../PrimeBox}
RBX3=${RBX3:-$REPO/../rbx3-firmware}
RX3=${RX3:-$RBX3/XDJRX3-rootfs}
PBSHIMS=$PRIMEBOX/scripts/shims
WORK=${WORK:-$REPO/work/rb/shims}
COMPAT=${COMPAT:-$REPO/work/rb/compat}

[ -d "$RX3" ]     || { echo "RX3 rootfs not found: $RX3" >&2; exit 1; }
[ -d "$PBSHIMS" ] || { echo "PrimeBox shims not found: $PBSHIMS" >&2; exit 1; }

echo "== stage shim sources in $WORK =="
mkdir -p "$WORK" "$COMPAT"
cp -a "$PBSHIMS/." "$WORK/"
cp "$REPO/scripts/rb/shims-Makefile" "$WORK/Makefile"
cp "$REPO/scripts/rb/audioshim.c"    "$WORK/audioshim.c"
cp "$REPO/scripts/rb/memshim.c"      "$WORK/memshim.c"
cp "$REPO/scripts/rb/keyshim.c"      "$WORK/keyshim.c"
cp "$REPO/scripts/rb/fbshim16-phone.c" "$WORK/fbshim16-phone.c"

# The RX3 runtime rootfs has no libc_nonshared.a / libpthread_nonshared.a
# (dev-only archives); give the linker empty stubs.
( cd "$COMPAT" && ar rcs libc_nonshared.a && ar rcs libpthread_nonshared.a )

LDFLAGS="-L$COMPAT -L$RX3/lib -L$RX3/usr/lib -Wl,-rpath-link,$RX3/lib:$RX3/usr/lib"
TARGETS=${*:-"memshim.so audioshim.so keyshim.so fbshim16.so"}

echo "== build: $TARGETS =="
# memshim must be a plain libc shim; audioshim links the RX3 libdl.so.2
make -C "$WORK" RX3="$RX3" LDFLAGS="$LDFLAGS" $TARGETS

echo "== GLIBC symbol versions (must be only 2.4 / 2.7) =="
for f in $TARGETS; do
    printf '%-18s ' "$f"
    arm-linux-gnueabi-objdump -T "$WORK/$f" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -u | tr '\n' ' '
    echo
done
echo
echo "deploy with: scripts/rb/deploy-module.sh   (or scp the .so into the chroot usr/lib)"
md5sum "$WORK"/*.so 2>/dev/null | grep -E 'memshim|audioshim'
