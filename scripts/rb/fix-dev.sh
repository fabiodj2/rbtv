#!/bin/sh
# fix-dev.sh — Chromebit (postmarketOS) chroot device binds + RX3 device stubs.
# Deployed to /home/user/fix-dev.sh. Run as root.
#
# The RX3 rootfs expects devices that do not exist on the Chromebit; rbp polls
# some of them, so they must exist or it busy-spins / aborts:
#   FIFOs (poll/read)   : subucom_spi*, hidg0
#   regular files (ioctl-only / polled GPIO): printkdrv0, tsc2007_2-0048, gpiodrv
# paudiog0 must be ABSENT so JUCE skips the gadget-audio ioctl path.
# /dev/mem is chmod 000 so rbp's i.MX6 register read fails cleanly (the
# audioshim mmap hook then turns the dangling MAP_SHARED/-1 into anon memory).
CH=/home/user/rbx3-run

for m in dev proc sys tmp; do
    umount "$CH/$m" 2>/dev/null
done

mkdir -p "$CH/dev" "$CH/proc" "$CH/sys" "$CH/tmp" "$CH/run"
mount --bind /dev  "$CH/dev"
mount --bind /proc "$CH/proc"
mount --bind /sys  "$CH/sys"
mount --bind /tmp  "$CH/tmp"

# RX3-style mount point for the rekordbox stick (bound by usb-watch.sh):
mkdir -p "$CH/media/usb1/sda1"

echo "fb0: $(ls -la "$CH/dev/fb0" 2>/dev/null | awk '{print $1}')"
echo "dev mounted: $(mountpoint -q "$CH/dev" && echo yes)"
echo "tmp mounted: $(mountpoint -q "$CH/tmp" && echo yes)"

# FIFOs for devices that threads poll/read (prevents 100% CPU busy-spin):
for d in subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0; do
    rm -f "$CH/dev/$d"
    mkfifo "$CH/dev/$d" 2>/dev/null || mknod "$CH/dev/$d" p
    chmod 666 "$CH/dev/$d" 2>/dev/null
done

# Regular-file stubs for ioctl-only devices and polled GPIOs:
for d in printkdrv0 tsc2007_2-0048 gpiodrv; do
    rm -f "$CH/dev/$d"
    : > "$CH/dev/$d"
    chmod 666 "$CH/dev/$d" 2>/dev/null
done

# Ensure gadget audio is NOT present:
rm -f "$CH/dev/paudiog0"

# Block /dev/mem (rbp must not read unmapped i.MX6 registers):
chmod 000 /dev/mem "$CH/dev/mem" 2>/dev/null

# udev FIFOs for USB stick detection (rb2go emulate-usb / usb-watch):
for f in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2; do
    [ -p "/tmp/$f" ] || { rm -f "/tmp/$f"; mkfifo "/tmp/$f"; chmod 666 "/tmp/$f"; }
done

# POSIX getmntent / vfs_getfsys:
ln -sf /proc/mounts "$CH/etc/mtab"

echo "stubs done"
