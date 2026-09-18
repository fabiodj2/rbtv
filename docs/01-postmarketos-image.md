# 01 — postmarketOS image: USB boot, logging, SSH and Wi-Fi

This document records what was changed on the upstream postmarketOS image so
the Chromebit boots from a USB stick unattended and is reachable over SSH.

* Upstream image: `20260904-1830-postmarketOS-edge-console-1-google-veyron.img.xz`
* Fixed image: produced locally from the upstream image (large binary, not shipped in this repo)
* Reproducible fix assets: [`../scripts/fix-image/`](../scripts/fix-image/)

Device: **ASUS Chromebit CS10 = `google-veyron-mickey`**, Rockchip RK3288,
armv7, postmarketOS edge (systemd, OpenSSH, NetworkManager).

## 1. Symptoms

Booting the USB stick (developer mode, **Ctrl+U**) landed in the initramfs
debug shell:

```
Run 'pmos_continue_boot' to continue booting.
```

The USB keyboard did not react, so nothing could be typed.

## 2. Root causes

1. **`usb-storage` was never loaded in the 1st-stage initramfs.**
   The module is present on the image, but it is not listed in
   `initramfs.load`, and `mdev` does not auto-load modules here. With no
   `/dev/sdX`, `blkid --uuid …` can never find the `pmOS_boot` / `pmOS_root`
   partitions. After 30 s `wait_partition()` called `fail_halt_boot()`, which
   enters the debug shell.
   (Booting from the built-in eMMC/SD does not need this, which is why the
   generic image normally works — a **USB stick does**.)

2. **`hid-generic.ko` was missing from the initramfs.**
   `CONFIG_USB_HID=y` and `CONFIG_HID=y` are built into the kernel, but
   `CONFIG_HID_GENERIC=m`. Without `hid-generic.ko` a generic USB keyboard
   produces no input events.

3. **The kernel cmdline had `quiet … loglevel=2`,** so the HDMI console showed
   almost nothing, and the Plymouth splash hid the rest.

## 3. Changes

### 3.1 Kernel partition (`pmOS_kernel`, partition 1 = `vmlinuz.kpart`)

Rebuilt with `depthchargectl` (the same tool postmarketOS/`boot-deploy` uses)
from the unchanged kernel + DTBs and a **patched 1st-stage initramfs**.

New kernel cmdline (see [`../scripts/fix-image/kernel-cmdline.txt`](../scripts/fix-image/kernel-cmdline.txt)):

```
kern_guid=%U loglevel=8 console=tty0 PMOS_NOSPLASH \
pmos_boot_uuid=be0bc07d-8f9e-4de9-8ef2-79cdf3d06252 \
pmos_root_uuid=9ec6c08d-efcc-4cdf-bed9-53b7761bb6ba \
pmos_rootfsopts=defaults
```

* `loglevel=8` + no `quiet` → all kernel messages on HDMI.
* `PMOS_NOSPLASH` → no Plymouth splash hiding the logs.
* Signed with the standard vboot devkeys (same key hash as the original, so it
  boots in developer mode exactly like the stock kpart).

### 3.2 Initramfs (`init`, `init_functions.sh`, `initramfs.load`)

Patches: [`init.patch`](../scripts/fix-image/init.patch),
[`init_functions.patch`](../scripts/fix-image/init_functions.patch),
[`initramfs.load.patch`](../scripts/fix-image/initramfs.load.patch).

* `load_usb_modules()` / `usb_hotplug_rescan()`: (re)load `usb-storage`,
  `uas`, `hid-generic`, `usbhid`, `evdev`; ask every SCSI host to rescan; run
  `mdev -s`. Called at boot **and repeatedly while waiting**, so plugging in a
  hub / keyboard / stick after boot is picked up.
* `wait_partition()` retries **forever** instead of dropping to the debug shell.
  Every 5 s it rescans USB and prints diagnostics
  (`blkid`, `/proc/partitions`, USB device list) to the console.
* `check_filesystem()` auto-continues after 10 s on fsck errors.
* `fail_halt_boot()` (only on truly fatal errors) dumps diagnostics and
  reboots after 30 s — no keyboard required.
* Console loglevel forced to 8 at startup.
* The debug shell is no longer entered automatically. Hold **Left-Ctrl** during
  boot to get it, or add `pmos.debug-shell` to the cmdline.
* `hid-generic.ko.zst` is added to the initramfs (upstream `modules.dep`
  already references it).

### 3.3 Boot partition (`pmOS_boot`, partition 2)

* `/boot/initramfs` replaced with the patched build.
* `/boot/extlinux/extlinux.conf` updated with the same verbose cmdline
  (for any extlinux/U-Boot path).

### 3.4 Root filesystem (`pmOS_root`, partition 3)

* **SSH enabled** (`openssh-server-pam` was installed but preset-disabled):
  `multi-user.target.wants/sshd.service` symlink and
  `/etc/systemd/system-preset/90-chromebit.preset`.
* `/etc/ssh/sshd_config.d/60-chromebit.conf`:
  `PasswordAuthentication yes`, `KbdInteractiveAuthentication yes`,
  `PermitRootLogin yes`.
* **Wi-Fi profile** `<SSID>` (WPA-PSK) in
  `/etc/NetworkManager/system-connections/`, `autoconnect` with unlimited
  retries. (NetworkManager is enabled in the image.)
* Hostname `chromebit` (`/etc/hostname`, `/etc/hosts`).

## 4. Credentials

| user | password |
|---|---|
| `root` | (postmarketOS default) |
| `user` | (postmarketOS default) |

These are the postmarketOS defaults — change them after first login.

```sh
ssh user@chromebit.local     # or root@…
```

On the same network the device is reachable as `chromebit.local` (mDNS).
Adjust the Wi-Fi `<SSID>` and address to your own setup.

## 5. Flashing

Apply the fixes in [`../scripts/fix-image/`](../scripts/fix-image/) to the
upstream image, then flash the **whole image** in raw/dd mode (not a
partition):

```sh
sudo dd if=pmos-veyron-chromebit-fixed.img of=/dev/sdX bs=4M \
        status=progress conv=fsync
```

Or select the `.img.xz` in balenaEtcher. On first boot the root partition is
grown to fill the stick automatically.

## 6. If it still does not boot

The HDMI console now shows everything, including:

```
[pmOS-rd] Waiting for boot partition (hot-plug friendly, retrying forever)...
[pmOS-rd] Still waiting for boot partition (5s)...
[pmOS-rd] --- blkid ---
…
```

Try unplugging/replugging the hub, keyboard or stick while it is retrying — the
initramfs keeps rescanning. Notes:

* The Chromebit USB-A port is USB 2.0; powered hubs help.
* Some sticks need a moment; the retry loop never gives up.

## 7. Wi-Fi notes

The pre-seeded profile is plain **WPA2-PSK**. If your network is really
**WPA2-Enterprise (802.1X)**, a password alone is not enough — you also need the
identity and EAP method. The easiest console tools are:

```sh
sudo nmtui-connect                       # interactive picker
nmcli device wifi connect "SSID" password "…"
```

See also the upstream postmarketOS wiki for the device:
<https://wiki.postmarketos.org/wiki/Google_Veyron_Chromebook_(google-veyron)>.

## 8. Making the fix survive `mkinitfs` / apk (device-side persistence)

Every `apk add`/upgrade runs postmarketOS's `mkinitfs`, which regenerates
`/boot/initramfs`, `/boot/initramfs-extra`, `vmlinuz.kpart` and
`extlinux.conf` **from the sources on the rootfs**, and reflashes the kernel
partition. A patched *image* alone is therefore not enough — the fix must live
in the device's sources:

| Source on the device | Effect |
|---|---|
| `/usr/share/initramfs/init.sh` | 1st-stage init (our `load_usb_modules` call, verbose printk) |
| `/usr/share/initramfs/init_functions.sh` | USB loading, retry-forever, hot-plug, USB port/controller reset |
| `/usr/share/mkinitfs/modules/00-device-google-veyron.modules` | both *copies* the listed `.ko` files **and** becomes `/usr/lib/modules/initramfs.load` (boot-time modprobe list) |
| `/etc/kernel-cmdline.d/50-device-google-veyron.conf` + empty `.../00-base.conf` | overrides the stock `quiet … loglevel=2` |

After changing them, run `mkinitfs` (it calls `boot-deploy`, rebuilds the kpart
and flashes it to `/dev/sda1`).

This is automated in [`../scripts/fix-image/persist-on-device.sh`](../scripts/fix-image/persist-on-device.sh):

```sh
CHROMEBIT=root@chromebit PASS=<password> ./scripts/fix-image/persist-on-device.sh
```

Verified by rebooting: the device came back up with
`[pmOS-rd] Found boot partition: /dev/sda2` at ~19.6 s.

## 9. USB enumeration and automatic recovery

The stick's **first** enumeration times out:

```
usb 1-1.4: new high-speed USB device number 4 using dwc2
usb 1-1.4: device descriptor read/64, error -110
usb 1-1.4: new high-speed USB device number 5 using dwc2   # ~12 s later
usb 1-1.4: New USB device found, idVendor=0781, idProduct=5581 (SanDisk Ultra)
```

The kernel's own retry usually saves it (~19 s), but occasionally it needed a
physical replug. To automate that, the initramfs now escalates while waiting
for the boot/root partition:

* `usb_port_reset()` — toggles the kernel USB-port `disable` attribute
  (`/sys/bus/usb/devices/*/*:*/*-port*/disable`), i.e. a **software
  unplug/replug**; called at 15 s and then every 30 s.
* `usb_controller_reset()` — rebinds the `dwc2` platform driver, resetting the
  whole bus; called at 30 s.
* Kernel cmdline adds `usbcore.old_scheme_first=1` (classic fix for
  `device descriptor read/64, error -110`) and `usbcore.autosuspend=-1`.

All of this is installed by the persistence step above, so it survives
`mkinitfs`.
