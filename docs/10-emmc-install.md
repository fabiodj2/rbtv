# 10 — Installing to the internal eMMC (no USB boot)

The Chromebit now boots the whole system (postmarketOS + the rekordbox chroot +
shims) from its **internal eMMC**, with no USB stick attached.

```
/       -> /dev/mmcblk0p3   (14.2 GiB, ext4, label pmOS_root)
/boot   -> /dev/mmcblk0p2   (512 MiB, ext2, label pmOS_boot)
cmdline -> pmos_root_uuid=ea318303-…  pmos_boot_uuid=c7611612-…
```

Script: [`../scripts/emmc-install.sh`](../scripts/emmc-install.sh).

## 1. Why you can't `dd`/`parted`/`fdisk` the eMMC

On Rockchip **Veyron** Chromebooks the eMMC's **first sectors are hardware
write-protected** (the PMBR + primary GPT). Any write there fails:

```
[2711.319226] I/O error, dev mmcblk0, sector 0 op 0x1:(WRITE) ...
```

so `cgpt create` dies with:

```
WARNING: Primary GPT header is being ignored
ERROR: I/O error when trying to write primary GPT
```

and a plain `dd` of a prebuilt image (or `pmbootstrap --no-cgpt`) cannot work.

The kernel and depthcharge cope by **ignoring the primary and using the
secondary GPT**:

```
[2753.039118] Primary GPT is being ignored, using alternate GPT.
[2753.039118]  mmcblk0: p1 p2 p3
```

## 2. The trick: `cgpt add`, never `cgpt create`

`cgpt`'s `GptSave()` skips the primary header when the drive is marked
"ignored". `cgpt create` *resets* that flag, so it then tries to write the
protected primary and fails. **`cgpt add` on the existing (secondary) GPT keeps
the flag and writes only the secondary** — which is exactly what
`pmbootstrap install --sdcard /dev/mmcblk0` does on veyron (see the pmOS wiki:
*"it handles that by ignoring primary GPT and using secondary instead"*).

```sh
cgpt add -i 1 -t kernel -b 24576   -s 32768    -l pmOS_kernel -S 1 -T 5 -P 10 /dev/mmcblk0
cgpt add -i 2 -t efi    -b 57344   -s 1048576  -l pmOS_boot   /dev/mmcblk0
cgpt add -i 3 -t data   -b 1105920 -s <rest>   -l pmOS_root   /dev/mmcblk0
```

The sizes come from the device package:
`/usr/share/deviceinfo/device-google-veyron` →
`deviceinfo_cgpt_kpart_start="24576"`, `deviceinfo_cgpt_kpart_size="32768"`.

## 3. Layout

| Part | Label | Start | Size | Type | Mount |
|---|---|---|---|---|---|
| p1 | `pmOS_kernel` | 24576 | 32768 (16 MiB) | ChromeOS kernel | — |
| p2 | `pmOS_boot` | 57344 | 1048576 (512 MiB) | EFI System Partition (ext2) | `/boot` |
| p3 | `pmOS_root` | 1105920 | rest (~14.1 GiB) | Linux data (ext4) | `/` |

## 4. Install procedure

`emmc-install.sh` does the whole thing **live from the USB-booted system**, so
the result contains *this exact machine* (chroot, `rbp`, shims, configs, the
`/home/user` payload — not a fresh image):

1. create the three partitions with `cgpt add`, `blockdev --rereadpt`
2. `mkfs.ext4` root, `mkfs.ext2` boot
3. stop `rbp`, unmount the chroot bind mounts (`dev/proc/sys/tmp`), then
   `tar` the running `/` (excluding `/proc /sys /dev /run /tmp /mnt`) onto the
   eMMC; `/boot` lands on p2
4. write `/etc/fstab` and `boot/extlinux/extlinux.conf` with the eMMC UUIDs
5. **repack the signed kernel** with the eMMC cmdline (the USB UUIDs are baked
   into the embedded config):

   ```sh
   printf "%s\n" "kern_guid=%U splash … pmos_boot_uuid=$BU pmos_root_uuid=$RO pmos_rootfsopts=defaults" > /tmp/cmdline.emmc
   vbutil_kernel --repack /tmp/vmlinuz.kpart.emmc \
       --signprivate /usr/share/vboot/devkeys/kernel_data_key.vbprivk \
       --keyblock    /usr/share/vboot/devkeys/kernel.keyblock \
       --version 1 --config /tmp/cmdline.emmc --oldblob /boot/vmlinuz.kpart
   ```

   (the devkeys are installed by pmOS; the running kpart uses the same key set)
6. `dd` the repacked kpart to `/dev/mmcblk0p1`, copy it to `/boot/vmlinuz.kpart`,
   and set the kernel partition priority: `cgpt add -i 1 -S 1 -T 5 -P 10`
7. `sync`, unmount, reboot **without** pressing Ctrl+U.

## 5. Verify

```sh
CHROMEBIT# findmnt -n -o SOURCE /        # /dev/mmcblk0p3
CHROMEBIT# lsblk -o NAME,SIZE,MOUNTPOINT | head
CHROMEBIT# cat /proc/cmdline | tr ' ' '\n' | grep uuid
```

Payload smoke test (same as docs/07–09):

```sh
CHROMEBIT# sh /home/user/start-rb.sh
# display: pan=0,0 + src-pos=1920x1080+0+0 ; audio: PCM RUNNING ; input: press-key.sh source
```

## 6. Notes / caveats

* **The USB stick is untouched.** If the eMMC kernel ever fails to boot, press
  **Ctrl+U** at the developer screen to boot the USB and re-run the install.
* **Updates**: re-running `emmc-install.sh` re-copies `/` and rebuilds the kpart
  (use `--copy-only` to skip `mkfs`, preserving nothing else). It always targets
  the same three partitions.
* The firmware's own flags aren't touched — this is still a developer-mode
  boot. The eMMC kernel partition has priority 10, so internal boot is the
  default; USB boot still needs the Ctrl+U gesture.
* Sector 0 (PMBR) stays as the firmware left it; only the secondary GPT is
  written. `cgpt show` will always print
  `WARNING: Primary GPT header is being ignored` — that is expected.
* The Chrome OS install (if any) on the eMMC is gone; to restore ChromeOS you'd
  need a recovery USB for `MINNIE`/`MICKEY`.

## 7. Changes made earlier that still apply

The USB-boot image had initramfs changes (load `usb-storage`/`hid-generic`,
retry USB forever, verbose cmdline). Those live in `/boot/initramfs*` and are
copied to the eMMC unchanged; they don't affect eMMC boot — the initramfs finds
the root by `pmos_root_uuid` (now the eMMC), which is present immediately, so
the USB retry path is simply never needed.
