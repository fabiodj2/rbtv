#!/bin/sh
# Persist the Chromebit postmarketOS USB-boot fix into the device's own
# initramfs *sources*, so it survives `mkinitfs` (which is triggered by every
# `apk add`/upgrade).
#
# Without this, installing any package on the device regenerates
# /boot/initramfs and vmlinuz.kpart from the stock sources and the USB-boot
# fix is lost (next reboot drops to the initramfs debug shell).
#
# Run on the WORKSTATION with the device reachable:
#   CHROMEBIT=root@chromebit PASS=<password> ./persist-on-device.sh
#
# The patched initramfs files live next to this script in ./patched/.
set -eu

HOST="${CHROMEBIT:-root@chromebit.local}"
PASS="${PASS:?set PASS to the device password}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCHED="$HERE/patched"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=15"
SP="sshpass -p $PASS"

[ -f "$PATCHED/init" ] || { echo "missing $PATCHED/init"; exit 1; }
[ -f "$PATCHED/init_functions.sh" ] || { echo "missing $PATCHED/init_functions.sh"; exit 1; }

echo ">> pushing patched initramfs sources"
$SP $SSH "$HOST" 'cat > /usr/share/initramfs/init.sh'            < "$PATCHED/init"
$SP $SSH "$HOST" 'cat > /usr/share/initramfs/init_functions.sh'  < "$PATCHED/init_functions.sh"

echo ">> installing cmdline overrides + module load list, then rebuilding"
$SP $SSH "$HOST" '
  set -e
  chmod 755 /usr/share/initramfs/init.sh
  chmod 644 /usr/share/initramfs/init_functions.sh

  # Verbose console + no splash (overrides /usr/lib/kernel-cmdline.d)
  mkdir -p /etc/kernel-cmdline.d
  printf "loglevel=8\nconsole=tty0\nPMOS_NOSPLASH\nplymouth.enable=0\nusbcore.old_scheme_first=1\nusbcore.autosuspend=-1\n" \
      > /etc/kernel-cmdline.d/50-device-google-veyron.conf
  : > /etc/kernel-cmdline.d/00-base.conf

  # These become both "copy this .ko" and the boot-time initramfs.load list
  M=/usr/share/mkinitfs/modules/00-device-google-veyron.modules
  for m in usb-storage uas hid-generic evdev; do
      grep -qx "$m" "$M" || echo "$m" >> "$M"
  done

  echo ">> cmdline:"; generate-kernel-cmdline
  echo ">> rebuilding initramfs + kpart"; mkinitfs
'

echo ">> done. /boot/initramfs, /boot/vmlinuz.kpart (and /dev/sda1) now carry the fix."
