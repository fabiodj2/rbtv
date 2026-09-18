#!/bin/sh
# emmc-install.sh — install the RUNNING postmarketOS (with the full rekordbox
# payload) onto the Chromebit's internal eMMC, so it boots without the USB stick.
#
# Run ON the Chromebit (as root), booted from the USB stick, with the eMMC free.
#
#   sh /home/user/emmc-install.sh            # partitions + copies + boots from eMMC
#   sh /home/user/emmc-install.sh --copy-only
#
# WHY NOT `dd`/`parted`: on Rockchip Veyron the eMMC's first sectors (PMBR +
# primary GPT) are HARDWARE WRITE-PROTECTED.  `cgpt create` fails with
# "I/O error when trying to write primary GPT".  `cgpt add` (without create)
# works because cgpt then keeps the existing primary marked "ignored" and writes
# only the SECONDARY GPT (which the kernel and depthcharge use:
#   dmesg: "Primary GPT is being ignored, using alternate GPT").
# This mirrors what `pmbootstrap install --sdcard /dev/mmcblk0` does on veyron.
#
# Layout (matches device-google-veyron deviceinfo):
#   p1 pmOS_kernel  24576 + 32768 (16 MiB)  ChromeOS kernel (signed kpart)
#   p2 pmOS_boot    57344 + 1048576 (512M)  ext2   /boot
#   p3 pmOS_root    rest                    ext4   /
set -eu

DEV=/dev/mmcblk0
KP_START=24576
KP_SIZE=32768
BO_START=57344
BO_SIZE=1048576
RO_START=1105920
KPART=${KPART:-/boot/vmlinuz.kpart}
DEVKEYS=/usr/share/vboot/devkeys
REPO_PAYLOAD=0

# ---------------------------------------------------------------- preflight
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -b "$DEV" ] || { echo "no $DEV" >&2; exit 1; }
grep -q "$DEV" /proc/mounts && { echo "$DEV is mounted — unmount first" >&2; exit 1; }

# Disk size in sectors -> root partition size (leave 33 for the backup GPT)
SECT=$(cat /sys/block/mmcblk0/size)
RO_SIZE=$(( SECT - RO_START - 33 ))
echo "disk=$SECT sectors  root part=$RO_SIZE sectors"

# ------------------------------------------------------------- 1. partitions
echo "== 1. cgpt partitions (secondary GPT only) =="
# NOTE: do NOT run `cgpt create` (it tries to write the WP primary and fails).
# Adding on top of the existing secondary GPT is what makes this work.
if ! cgpt show "$DEV" 2>/dev/null | grep -q '"pmOS_kernel"'; then
    cgpt add -i 1 -t kernel -b "$KP_START" -s "$KP_SIZE" -l pmOS_kernel -S 1 -T 5 -P 10 "$DEV"
fi
if ! cgpt show "$DEV" 2>/dev/null | grep -q '"pmOS_boot"'; then
    cgpt add -i 2 -t efi  -b "$BO_START" -s "$BO_SIZE" -l pmOS_boot "$DEV"
fi
if ! cgpt show "$DEV" 2>/dev/null | grep -q '"pmOS_root"'; then
    cgpt add -i 3 -t data -b "$RO_START" -s "$RO_SIZE" -l pmOS_root "$DEV"
fi
blockdev --rereadpt "$DEV" 2>/dev/null || true
sleep 1
ls -l ${DEV}p1 ${DEV}p2 ${DEV}p3

# ----------------------------------------------------------------- 2. format
echo "== 2. format =="
if [ "${1:-}" != "--copy-only" ]; then
    mkfs.ext4 -q -F -L pmOS_root -E lazy_itable_init=1 ${DEV}p3
    mkfs.ext2 -q -F -L pmOS_boot ${DEV}p2
fi

mkdir -p /mnt/emmc
mount ${DEV}p3 /mnt/emmc
mkdir -p /mnt/emmc/boot
mount ${DEV}p2 /mnt/emmc/boot

BU=$(blkid -s UUID -o value ${DEV}p2)
RO=$(blkid -s UUID -o value ${DEV}p3)
echo "boot UUID=$BU  root UUID=$RO"

# ------------------------------------------------------------- 3. copy root
echo "== 3. copy the running system =="
pkill -x ld-linux.so.3 2>/dev/null || true
sleep 1
for m in dev proc sys tmp; do umount /home/user/rbx3-run/$m 2>/dev/null || true; done
tar -C / \
    --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run \
    --exclude=./tmp --exclude=./mnt \
    -cf - . | tar -C /mnt/emmc -xf -
sync

# --------------------------------------------------------- 4. boot config
echo "== 4. fstab + extlinux =="
printf "UUID=%s / ext4 defaults 0 0\nUUID=%s /boot ext2 nodev,nosuid,noexec 0 0\n" \
       "$RO" "$BU" > /mnt/emmc/etc/fstab
OLDBU=$(sed -n 's/.*pmos_boot_uuid=\([0-9a-f-]*\).*/\1/p' /mnt/emmc/boot/extlinux/extlinux.conf | head -1)
OLDRO=$(sed -n 's/.*pmos_root_uuid=\([0-9a-f-]*\).*/\1/p' /mnt/emmc/boot/extlinux/extlinux.conf | head -1)
[ -n "$OLDBU" ] && [ -n "$OLDRO" ] && \
    sed -i "s/$OLDBU/$BU/g; s/$OLDRO/$RO/g" /mnt/emmc/boot/extlinux/extlinux.conf

# --------------------------------------------------- 5. signed kernel kpart
echo "== 5. repack the kernel with the eMMC cmdline =="
printf "%s\n" "kern_guid=%U splash plymouth.ignore-serial-consoles plymouth.prefer-fbcon loglevel=8 console=tty0 PMOS_NOSPLASH plymouth.enable=0 usbcore.old_scheme_first=1 usbcore.autosuspend=-1 pmos_boot_uuid=$BU pmos_root_uuid=$RO pmos_rootfsopts=defaults" > /tmp/cmdline.emmc
vbutil_kernel --repack /tmp/vmlinuz.kpart.emmc \
    --signprivate "$DEVKEYS/kernel_data_key.vbprivk" \
    --keyblock   "$DEVKEYS/kernel.keyblock" \
    --version 1 --config /tmp/cmdline.emmc --oldblob "$KPART"
dump_kernel_config /tmp/vmlinuz.kpart.emmc | tr ' ' '\n' | grep -E 'uuid|kern_guid'

echo "== 6. write kernel partition + priority =="
dd if=/tmp/vmlinuz.kpart.emmc of=${DEV}p1 bs=1M conv=fsync
cp /tmp/vmlinuz.kpart.emmc /mnt/emmc/boot/vmlinuz.kpart
cgpt add -i 1 -S 1 -T 5 -P 10 "$DEV"

sync
umount /mnt/emmc/boot
umount /mnt/emmc
echo
echo "DONE.  Reboot (do NOT press Ctrl+U) to boot from eMMC."
echo "Fallback if it does not boot: press Ctrl+U at the dev screen to boot the USB."
