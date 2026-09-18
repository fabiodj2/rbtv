# 11 — USB stick & rekordbox database detection

A **real rekordbox USB stick** connected to the Chromebit (directly or through
the USB2.1 hub) is detected, mounted, registered as **USB1** in `rb`, and its
rekordbox database (`export.pdb`) is imported by the native DeviceSQL engine —
so the player shows the full library (songs, playlists, categories), not just a
folder listing.

> This is the *device* path, not rb2go's "emulate a folder as a stick". The
> stick is a real block device; the volume label, capacity and free space come
> from the filesystem, and the song/playlist counts come from Pioneer's own
> database import.

Status: **working.** Verified on hardware with a 57 GiB vfat stick
(`MY MUSIC`, `export.pdb` 3.4 MB): `detect=2`, **2174 songs, 160 playlists**,
`uiConnectedMedia = 0x2`, and the `SELECT` key opens the database
(DATE ADDED → YEAR/MONTH + the full category sidebar).

## 1. Hardware topology

The Chromebit CS10 has a single USB-A port wired to the RK3288 **dwc2 OTG
controller in host mode**:

```
/sys/devices/platform/ff580000.usb/usb1/          <- root hub "usb1"
   └── 1-1           05e3:0610 USB2.1 hub
        ├── 1-1.1     0781:5581 SanDisk / rekordbox stick   -> /dev/sda
        └── 1-1.x     1a2c:7b81 USB keyboard
```

* There is **no gadget port** (`dr_mode = "host"`), so the USB-device-emulation
  trick used on other hardware is not available. The stick is handled by the
  *filesystem* path, exactly like the XDJ-RX3 does for its own USB1 slot.
* The watcher matches the root-hub segment `"/usb1/"` rather than a fixed
  device name, so it works with a hub, without a hub, and across re-enumeration
  (`sda`/`sdb`/…).
* Root runs from the internal eMMC (`mmcblk0p3`), so a stick is never the boot
  device; the watcher still refuses any disk that carries `/` or `/boot`.

### Several storage devices on the port at once

A hub often exposes more than one disk: the stick *and* a card reader (which
reports **size 0** when no card is inserted).  Picking the first `sd*` would
latch onto the empty reader and never look at the stick, so the watcher builds
all candidates and ranks them:

1. a device carrying `PIONEER/rekordbox/export.pdb` (the real library) wins —
   checked by a **read-only** mount, or by looking at the existing mount point
   when it is already in use;
2. otherwise the first candidate with a recognised filesystem is used (rbp then
   shows the FOLDER view);
3. zero-size devices (no medium) and disks carrying `/`/`/boot` are skipped.

The pick is re-evaluated whenever the set of storage devices changes, so
plugging the stick in *after* the reader is detected still works.

```sh
CHROMEBIT# sh /home/user/usb-watch.sh status
candidates:  sdb
stick:       sdb
```

### Boot autostart

`rb.service` (`scripts/rb/device/rb.service`) runs `start-rb.sh` on every boot:
the player, the DDJ-400 bridge and the keyboard daemon come up by themselves,
and so does this watcher — no SSH needed after a reboot.

```sh
CHROMEBIT# systemctl restart rb.service     # full player restart
CHROMEBIT# systemctl status rb.service
```

## 2. The detection chain

This is the stock XDJ-RX3 udev → rbp chain, with our watcher replacing udev:

```
stick inserted
  → usb-watch.sh mounts  /dev/sdX1 -> /media/usb1/sda1      (RX3 vfat options)
  → bind-mounts it into the chroot  .../rbx3-run/media/usb1/sda1
  → writes "umount /media/usb1/sda1" then
           "mount  /media/usb1/sda1"  to  /tmp/udev_usb1
        │
        ▼  ui::UsbMountManager::run()          (thread reads the FIFO)
        ▼  ui::UsbStorageManager::notify_usb_mount()
        ▼  ui::DbProxy::reqAttach()
        ▼  db::DbIF::mount(path, type=3)
             • vfs_setfsys('C', vfat, "/media/usb1/sda1", 0)
             • posts mailbox msg 11 -> Total_MainTASK
        ▼  Total_MainUsbMessageProc (case 11)
             • SetMountInfo_DriveLetter(0, 3, 'C')
             • SetMountInfo_DeviceDetectFlg(0, 3, 1)      (analysing)
        ▼  DeviceSQL (edb_streamd) imports PIONEER/rekordbox/export.pdb
        ▼  detect flag (drive 0, media kind 2) = 2        (ready)
        ▼  compConnectedMedia() -> uiConnectedMedia bit1  (USB 1)
        ▼  SOURCE screen shows "USB1 <volume label>" + Songs/Playlists/…
```

### Why media kind 2 is "USB1"

The engine inherited the CDJ-3000 media model. USB1 in the UI is **kind 2**
(the SD-slot kind in the original model); USB2 is kind 3.

| Kind | Detect flag | Property block | UI |
|---|---|---|---|
| 2 | `0x03256888` | `0x0325688c` (168 B) | **USB 1** |
| 3 | `0x03256944` | `0x03256948` (168 B) | USB 2 |

The Chromebit has only one watched port, so kind 3 stays absent and there is no
phantom "USB2" (the flag is only ever set from a mount event).

### `DevicePropertyInfo` (`+0` of the property block)

| Offset | Type | Meaning |
|---|---|---|
| +0 | UTF-16LE | volume label |
| +64 | — | date string |
| +120 | u32 | song count |
| +124 | u8 | background colour |
| +126 | u8 | database ready flag |
| +128 | u32 | playlist count |
| +132 | u32 | total capacity **high** word |
| +136 | u32 | total capacity **low** word |
| +140 | u32 | free space high word |
| +144 | u32 | free space low word |

All of it is filled natively by DeviceSQL reading the stick — nothing is
synthesised by a shim.

## 3. Required patches (already in `rbp`)

The Chromebit build (`work/rb/rbp-chromebit`, md5 `18a64bc4…`) already carries
the Prime GO patch set (`PrimeBox/tools/patch-rbp/rbp_patch.py`), which includes
everything the USB path needs:

* **udev FIFO paths** — `/proc/udev_usb1|2`, `/proc/udev_usbctn1|2` →
  `/tmp/udev_*` (procfs cannot hold a FIFO; `/tmp` is shared with the chroot).
* **NULL power-manager guards** — without a Pioneer PM MCU the mount path would
  dereference NULL and kill the `UsbMountManager` thread:
  `notifyPermissionChanged` (`0x2c6bf0`) and `notifyPreparedToStandby`
  (`0x2c7004`) → `bx lr`, plus the `0x121d9c…`, `0x32e728`, `0x3871d0` helpers.

Verify on the device:

```sh
CHROMEBIT# strings /home/user/rbx3-run/root/pdj/rbp | grep udev
/tmp/udev_usb1
/tmp/udev_usb2
/tmp/udev_usbctn1
/tmp/udev_usbctn2
```

## 4. `usb-watch.sh`

`scripts/rb/device/usb-watch.sh` is a small hotplug daemon (no udev rules, no
systemd dependency). On start it also serves as the notification bridge for
`rbp` restarts.

```sh
CHROMEBIT# sh /home/user/usb-watch.sh start     # background
CHROMEBIT# sh /home/user/usb-watch.sh status    # state + live probe
CHROMEBIT# sh /home/user/usb-watch.sh stop
CHROMEBIT# sh /home/user/usb-watch.sh run       # foreground
```

Attach does:

1. find `sd*` under `/usb1/` (skipping system disks), wait for the first
   partition (or fall back to a whole-disk filesystem);
2. mount with the RX3 options
   `flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,codepage=437,iocharset=iso8859-1,usefree,utf8`
   (`exfat`/`hfsplus`/generic fallbacks too);
3. `mount --bind` into the chroot at `/home/user/rbx3-run/media/usb1/sda1`;
4. write `umount …` **then** `mount …` to `/tmp/udev_usb1`
   (`rbp`'s `PathDecider` ignores a `mount` not preceded by an `umount`);
5. **re-send the mount event until the database is ready** — see below.

### The confirm loop (important)

A `mount` event written while `rbp` is still starting up is **silently
dropped**: the FIFO reader thread exists, so the write succeeds, but the DB
subsystem is not ready yet and the import never starts. This caused a cold start
to show `detect=0` even though everything else was correct.

The watcher therefore keeps re-notifying (`umount` + `mount`) every 5 s, up to
`USBWATCH_CONFIRM_TRIES` (default 18 → 90 s), and stops as soon as the detect
flag reads 2. The retry lives in the single-threaded run loop, so two writers
can never interleave FIFO messages. A re-notification after `rbp` restarts is
handled the same way.

### Never write to `/tmp/udev_usbctn*`

A "connect" event on the `udev_usbctn*` FIFOs triggers the (cosmetic)
**"USB Error. Remove the device."** caution. The mount event alone is enough.

## 5. `usb-probe.sh`

`scripts/rb/device/usb-probe.sh` reads `rbp`'s live mount-info tables through
`/proc/<pid>/mem` (root) and prints the real detection state:

```
rbp pid:     5458
mount:       /media/usb1/sda1 <- /dev/sda1 (vfat)
label:       MY MUSIC
export.pdb:  3448832 bytes
chroot bind: yes
kind 2 USB1: detect=2 (READY)  label='My Music' songs=2174   playlists=160   db_ready=1  cap=61.5GB free=12.8GB
kind 3 USB2: detect=0 (absent)  label=''             songs=0      playlists=0     db_ready=0  cap=0.0GB free=0.0GB
uiConnectedMedia = 0x2 (bit1 USB1: set)   uiBrowse = 12
```

`detect` is `0` absent, `1` analysing, `2` ready. Exit status is 0 when USB1 is
ready (usable as a health check). `uiConnectedMedia` bit1 only updates while the
UI is on the source screen (`uiBrowse == 12`); it is `0` on the deck view even
though the drive is ready.

## 6. Where it is wired in

| File | Change |
|---|---|
| `scripts/rb/fix-dev.sh` | creates `$CH/media/usb1/sda1`; ensures the `/tmp/udev_*` FIFOs |
| `scripts/rb/start-rb.sh` | after launching `rbp`, runs `usb-watch.sh start` |
| `scripts/rb/deploy-module.sh` | ships `usb-watch.sh` / `usb-probe.sh` |
| `scripts/rb/deploy-chromebit.sh` | ships the same + installs `rb-usbwatch.service` |
| `scripts/rb/device/rb-usbwatch.service` | optional boot-time unit (`systemctl enable --now rb-usbwatch`) |

`start-rb.sh` is the normal launch path, so the watcher starts with the player
and survives `rbp` restarts (it re-notifies). The systemd unit is for when `rb`
itself becomes a service.

## 7. End-to-end verification

Cold start (stick already inserted, `rbp` freshly launched):

```
21:30:45 attach: detected sda on watched port
21:30:45 attach: mounted /dev/sda1 (vfat label=MY MUSIC) -> /media/usb1/sda1
21:30:45 attach: chroot bind ok (/home/user/rbx3-run/media/usb1/sda1)
21:30:47 notify: umount /media/usb1/sda1
21:30:47 notify: mount /media/usb1/sda1
21:30:47 attach: notified native mount /media/usb1/sda1
21:30:54 confirm: not ready yet, re-notifying mount (17 left)
21:30:54 notify: umount /media/usb1/sda1
21:30:54 notify: mount /media/usb1/sda1
21:30:55 confirm: USB1 database READY
```

Hotplug (driver unbind ⇒ unplug, bind ⇒ replug) was also verified: the watcher
unmounts, sends `umount`, and re-attaches + re-imports on replug.

With the UI on the source screen and `SELECT` pressed, the database opens:

* **SOURCE** → `USB1  My Music`, Songs 2174, Playlists 160, Date
  2024-11-23, Total 57.3 GB, Available 11.9 GB.
* **SELECT** → `DATE ADDED` view with YEAR/MONTH columns and the category
  sidebar (ARTIST, ALBUMS, TRACK, KEY, PLAYLIST, HISTORY, MATCHING, FOLDER).

(No vendor UI screenshots are included in this repository.)

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `detect=0`, stick mounted | `mount` event sent before rbp's DB subsystem was ready | the confirm loop re-sends it; check `usbwatch.log` for `confirm: USB1 database READY` |
| `detect` never leaves `1` | `edb_streamd` not running / crashed | `start-rb.sh` starts it; remove stale `/tmp/{guard,req}_LocalDBServer` and restart |
| UI shows the drive but 0 songs | `export.pdb` missing or unreadable | check `$MNT/PIONEER/rekordbox/export.pdb`; DeviceSQL rewrites it on import |
| `881466368.0 GB`-style capacity | capacity words swapped | not applicable — capacity is filled natively from the filesystem |
| `uiConnectedMedia = 0` while ready | deck view (`uiBrowse != 12`) | press SOURCE; the mask is computed on the source screen |
| "USB Error. Remove the device." | something wrote to `/tmp/udev_usbctn*` | this watcher never does; do not echo to those FIFOs |
| stick not found | hub/port path differs | set `USBWATCH_SEG` (e.g. `USBWATCH_SEG="/usb1/"`) |
| FIFO write blocks | no reader (rbp down) and no `timeout` binary | the watcher's fallback kills the writer after 3 s |

## 9. Files

```
scripts/rb/device/usb-watch.sh          hotplug daemon: mount + bind + FIFO notify + confirm
scripts/rb/device/usb-probe.sh          live detect-flag / DB probe (/proc/pid/mem)
scripts/rb/device/rb-usbwatch.service   optional systemd unit
scripts/rb/fix-dev.sh                   creates media mount point + udev FIFOs
scripts/rb/start-rb.sh                  starts the watcher with rbp
```
