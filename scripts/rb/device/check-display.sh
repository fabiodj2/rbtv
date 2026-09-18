#!/bin/sh
# check-display.sh — run ON the Chromebit (as root) to diagnose the fb/HDMI path
# without starting rbp.  Shows exactly which buffer the VOP is scanning, whether
# the driver published, and whether the pixels are where the scanout looks.
#
# Usage:  sh check-display.sh
CH=/home/user/rbx3-run

echo "== fb0 =="
cat /sys/class/graphics/fb0/name
printf 'virtual_size=';  cat /sys/class/graphics/fb0/virtual_size
printf 'bpp=';           cat /sys/class/graphics/fb0/bits_per_pixel
printf 'stride=';        cat /sys/class/graphics/fb0/stride
printf 'pan=';           cat /sys/class/graphics/fb0/pan

echo
echo "== connector =="
printf 'status='; cat /sys/class/drm/card0-HDMI-A-1/status
printf 'dpms=';   cat /sys/class/drm/card0-HDMI-A-1/dpms 2>/dev/null

echo
echo "== VOP plane (what the TV shows) =="
grep -E 'plane\[|fb=|crtc-pos|src-pos' /sys/kernel/debug/dri/0/state | head -12
echo "  ^ src-pos y MUST be +0 (the RK3288 VOP does not present a panned buffer)"

echo
echo "== driver publish log (last run) =="
grep -E 'FLIP|UPDATE' /tmp/flipdbg.log 2>/dev/null | head -12 || echo "  (none — driver never published)"

echo
echo "== chroot pieces =="
echo -n 'driver md5: '; md5sum "$CH/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so" 2>/dev/null | cut -d' ' -f1
echo -n 'rbp md5:    '; md5sum "$CH/root/pdj/rbp" 2>/dev/null | cut -d' ' -f1
echo "expected driver 381f11946cc32064c65a5b7ade095497  (or newer)"
echo "expected rbp    18a64bc4d0ffd1cbd35f3a6ea447fca8 (rbp-audio + getPcController guard)"

echo
echo "== nonzero bytes in each physical buffer (first 400k) =="
for i in 0 1 2; do
    printf 'buffer %s: ' "$i"
    dd if=/dev/fb0 bs=8294400 skip=$i count=1 2>/dev/null | head -c 400000 | tr -d '\0' | wc -c
done
echo "  the scanning buffer (src-pos y / 1080) should be the non-empty one"
