# Patched postmarketOS initramfs files

These are the **modified** postmarketOS initramfs source files that
[`../persist-on-device.sh`](../persist-on-device.sh) installs on the device:

| File here | Role | Unmodified base / diff |
|---|---|---|
| `init` | installed as `/usr/share/initramfs/init.sh` | [`../init.patch`](../init.patch) |
| `init_functions.sh` | installed as `/usr/share/initramfs/init_functions.sh` | [`../init_functions.patch`](../init_functions.patch) |
| `initramfs.load` | module list used by `mkinitfs` | [`../initramfs.load.patch`](../initramfs.load.patch) |

Each file also carries a short provenance header added at the top, so it
differs from the raw patch output by that comment.

## Provenance

Derived from the postmarketOS initramfs package **`postmarketos-initramfs`**
(version observed on the device: `3.12.3-r1`). Upstream source:

* <https://gitlab.com/postmarketOS/pmaports/-/tree/master/main/postmarketos-initramfs>
  (files `init.sh`, `init_functions.sh`)
* package: <https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/postmarketos-initramfs>

The base files are © the postmarketOS contributors.

## Licence

The postmarketOS initramfs is licensed **GPL-2.0-or-later**. These modified
copies — and the patches in the parent directory that derive from them — are
therefore also **GPL-2.0-or-later**. The full GPL-2.0 text is in
[`COPYING`](COPYING).

The rest of this repository (everything not derived from the postmarketOS
initramfs) is MIT; see [`/LICENSE`](../../../LICENSE). The GPL files are merely
aggregated with the MIT parts and keep their own licence.

## Local changes

All changes are the Chromebit USB-boot reliability fixes documented in
[`../../../docs/01-postmarketos-image.md`](../../../docs/01-postmarketos-image.md):

* **`init`** — raise the console log level at the very start (`dmesg -n 8`,
  `printk = 8 4 1 7`) and load the USB modules before waiting for the boot
  partition.
* **`init_functions.sh`**
  * new `load_usb_modules()`, `usb_hotplug_rescan()`, `usb_port_reset()` and
    `usb_controller_reset()` — load `usb-storage`/`uas`/`hid-generic`/`usbhid`/
    `evdev`, rescan SCSI hosts, and recover a device whose enumeration timed
    out by a software "replug" (per-port `disable` toggle) or a `dwc2`
    controller rebind;
  * wait for the boot partition **forever**, rescanning and printing
    `blkid` / `/proc/partitions` / USB diagnostics, instead of dropping to an
    interactive debug shell (which needs a keyboard);
  * `check_filesystem()` — continue automatically after a short delay on an
    fsck warning instead of waiting for a keypress;
  * `fail_halt_boot()` — dump diagnostics and auto-reboot after 30 s instead of
    waiting for a keypress.
* **`initramfs.load`** — add `usb-storage`, `uas`, `hid-generic`, `evdev` to the
  modules copied into and loaded by the initramfs.
