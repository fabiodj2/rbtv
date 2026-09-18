#!/bin/sh
# build-directfb.sh — build the DirectFB 1.4.16 soft-float stack for the
# Chromebit (RK3288, HDMI, 32bpp rockchipdrmfb) and stage the result.
#
# Produces, in $OUT (default work/rb/dfb):
#   lib/libdirectfb-1.4.so.0.0.0   (+ symlink .so.0)
#   lib/libdirect-1.4.so.0.0.0     (+ symlink .so.0)
#   lib/libfusion-1.4.so.0.0.0     (+ symlink .so.0)
#   lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
#   lib/directfb-1.4-6/wm/libdirectfbwm_default.so
#   lib/directfb-1.4-6/inputdrivers/libdirectfb_{linux_input,keyboard}.so
#
# Base: PrimeBox's DirectFB diff (RK3288 triple-buffer / 32bpp fixes) plus
# scripts/rb/directfb-chromebit.patch, which makes the software scale+convert
# path run with rotation off (HDMI needs RGB565->RGB32 scaling, not rotation).
#
# Requires: git, gcc-arm-linux-gnueabi, libc6-dev-armel-cross, autoconf,
#           automake, libtool, patchelf.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
PRIMEBOX=${PRIMEBOX:-$REPO/../PrimeBox}
RBX3=${RBX3:-$REPO/../rbx3-firmware}
RX3=${RX3:-$RBX3/XDJRX3-rootfs}
DFBDIFF=${DFBDIFF:-$PRIMEBOX/tools/build-directfb/directfb-full.diff}
OURPATCH=$REPO/scripts/rb/directfb-chromebit.patch
NEONPATCH=$REPO/scripts/rb/directfb-chromebit-neon.patch
SYS=${SYS:-/tmp/arm213sysroot}
BUILD=${BUILD:-/tmp/dfb}
OUT=${OUT:-$REPO/work/rb/dfb}

[ -d "$RX3" ]     || { echo "RX3 rootfs not found: $RX3"; exit 1; }
[ -f "$DFBDIFF" ] || { echo "PrimeBox diff not found: $DFBDIFF"; exit 1; }
[ -f "$OURPATCH" ]|| { echo "our patch not found: $OURPATCH"; exit 1; }
[ -f "$NEONPATCH" ]|| { echo "NEON patch not found: $NEONPATCH"; exit 1; }

echo "== 1. soft-float sysroot ($SYS)"
sudo rm -rf "$SYS"
mkdir -p "$SYS/lib" "$SYS/usr/lib" "$SYS/usr/include"
cp -a "$RX3/lib/."          "$SYS/lib/"
cp -a "$RX3/usr/lib/."      "$SYS/usr/lib/"
cp -a /usr/arm-linux-gnueabi/include/. "$SYS/usr/include/"
cp /usr/arm-linux-gnueabi/lib/libc_nonshared.a "$SYS/usr/lib/libc_nonshared.a"
arm-linux-gnueabi-gcc -O2 -march=armv5t -mfloat-abi=soft -fPIC \
    -c "$REPO/scripts/rb/compat_nonshared.c" -o /tmp/compat_nonshared.o
ar r  "$SYS/usr/lib/libc_nonshared.a" /tmp/compat_nonshared.o
ar rcs "$SYS/usr/lib/libpthread_nonshared.a"

echo "== 2. DirectFB 1.4.16 + patches ($BUILD)"
rm -rf "$BUILD"
git clone -q https://github.com/deniskropp/DirectFB.git "$BUILD"
cd "$BUILD"
git checkout -q origin/directfb-1.4        # == 1.4.16
patch -p1 --quiet < "$DFBDIFF"
patch -p1 --quiet < "$OURPATCH"
patch -p1 --quiet < "$NEONPATCH"
printf 'int dfb_fbdev_compat_shim(void){return 0;}\n' > systems/fbdev/compat_shim.c

echo "== 3. configure"
export CC=arm-linux-gnueabi-gcc CXX=arm-linux-gnueabi-g++
export CFLAGS="-march=armv5t -mfloat-abi=soft --sysroot=$SYS"
export CPPFLAGS="--sysroot=$SYS"
export LDFLAGS="--sysroot=$SYS -L$SYS/usr/lib -L$SYS/lib -Wl,-rpath-link,$SYS/lib:$SYS/usr/lib"
./autogen.sh --host=arm-linux-gnueabi --build=x86_64-linux-gnu --prefix=/usr \
    --disable-x11 --disable-sdl --disable-vnc --disable-avifile \
    --with-gfxdrivers=none --disable-osx --disable-devmem \
    --disable-freetype --disable-png --disable-jpeg --disable-gif \
    --disable-tiff --disable-libmpeg3 --disable-imlib2 \
    --disable-video4linux --disable-video4linux2 --disable-dvb --disable-alsa \
    > /tmp/dfb-configure.log 2>&1 || { tail -30 /tmp/dfb-configure.log; exit 1; }

echo "== 4. build"
make -j"$(nproc)" LDFLAGS="$LDFLAGS" > /tmp/dfb-make.log 2>&1 || {
    grep -iE "error:|undefined reference" /tmp/dfb-make.log | head -20; exit 1; }

echo "== 5. stage + soname fixups -> $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/lib/directfb-1.4-6/systems" \
         "$OUT/lib/directfb-1.4-6/wm" \
         "$OUT/lib/directfb-1.4-6/inputdrivers"
cp src/.libs/libdirectfb-1.4.so.6.0.10        "$OUT/lib/libdirectfb-1.4.so.0.0.0"
cp lib/direct/.libs/libdirect-1.4.so.6.0.10   "$OUT/lib/libdirect-1.4.so.0.0.0"
cp lib/fusion/.libs/libfusion-1.4.so.6.0.10   "$OUT/lib/libfusion-1.4.so.0.0.0"
cp systems/fbdev/.libs/libdirectfb_fbdev.so   "$OUT/lib/directfb-1.4-6/systems/"
cp wm/default/.libs/libdirectfbwm_default.so  "$OUT/lib/directfb-1.4-6/wm/"
cp inputdrivers/linux_input/.libs/libdirectfb_linux_input.so \
   inputdrivers/keyboard/.libs/libdirectfb_keyboard.so \
   "$OUT/lib/directfb-1.4-6/inputdrivers/"

cd "$OUT/lib"
patchelf --set-soname libdirectfb-1.4.so.0 libdirectfb-1.4.so.0.0.0
patchelf --set-soname libdirect-1.4.so.0   libdirect-1.4.so.0.0.0
patchelf --set-soname libfusion-1.4.so.0   libfusion-1.4.so.0.0.0
# NOTE: the CORE libs must be rewritten too. libfusion links libdirect-1.4.so.6
# and is dlopen'd by libdirectfb, so leaving it unrewritten makes the first
# --list/exec fail with "libdirect-1.4.so.6: cannot open shared object file".
for f in libdirectfb-1.4.so.0.0.0 libdirect-1.4.so.0.0.0 libfusion-1.4.so.0.0.0 \
         directfb-1.4-6/systems/libdirectfb_fbdev.so \
         directfb-1.4-6/wm/libdirectfbwm_default.so \
         directfb-1.4-6/inputdrivers/*.so; do
    for s in libdirect-1.4.so.6 libfusion-1.4.so.6 libdirectfb-1.4.so.6; do
        patchelf --replace-needed "$s" "${s%.6}.0" "$f" 2>/dev/null || true
    done
done
ln -sf libdirectfb-1.4.so.0.0.0 libdirectfb-1.4.so.0
ln -sf libdirect-1.4.so.0.0.0   libdirect-1.4.so.0
ln -sf libfusion-1.4.so.0.0.0   libfusion-1.4.so.0

echo "== done. ABI check:"
for f in libdirectfb-1.4.so.0.0.0 libdirect-1.4.so.0.0.0 libfusion-1.4.so.0.0.0; do
    printf '%-45s ' "$f"
    arm-linux-gnueabi-objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -u | tr '\n' ' '
    echo
done
