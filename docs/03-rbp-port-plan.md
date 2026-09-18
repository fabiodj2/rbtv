# 03 — Port plan: `rb` (XDJ-RX3 rekordbox) on the Chromebit

This is the subsystem-by-subsystem plan. It reuses the finished PrimeBox port
(Prime GO, same RK3288) and rb2go (postmarketOS glue), changing only the
device-specific layers.

Paths used below:

* `$REPO` — this repository
* `$PRIMEBOX` — a checkout of <https://github.com/erhan-/PrimeBox>
  (firmware tooling + shim sources)
* `$RBX3` — your extracted XDJ-RX3 firmware workdir (ISO tree + soft-float rootfs)
* `CHROMEBIT#` — commands on the device (root via `sudo -S`, or `root` login)
* chroot root on device: `/home/user/rbx3-run`

> ⚠️ Running `rbp` on a mismatched display stack has historically hard-locked
> devices. Test the display path first and keep a second SSH session open.

## 0. Firmware, extraction, patching (workstation)

Reuse the existing working tooling — do **not** re-derive:

```sh
WORKSTATION$ cd $PRIMEBOX
WORKSTATION$ ./tools/get-firmware.sh ~/xdjrx3-fw        # official v1.20
# put the key (from AlphaTheta's GPL source distribution) at keys/aes256.key
WORKSTATION$ (cd tools/rx3dec && cargo build --release)
WORKSTATION$ ./tools/rx3dec/target/release/rx3dec \
      ~/xdjrx3-fw/XDJ-RX3_v120/XDJ-RX3.UPD keys/aes256.key \
      extracted/XDJRX3.iso
WORKSTATION$ 7z x extracted/XDJRX3.iso -oextracted/XDJRX3
WORKSTATION$ python3 tools/patch-rbp/rbp_patch.py \
      extracted/XDJRX3/pdj/rbp -o extracted/rbp-audio
```

Extract the **rootfs** (`rootfs.cramfs`), **gui** (`gui.tar.gz`) and the ISO
tree (`lib/`, `usr/`) as described in PrimeBox `TUTORIAL.md` Parts A3–A5. The
result is the soft-float userland used by the chroot.

## 1. Soft-float chroot on the Chromebit

The Chromebit runs **postmarketOS (musl, hard-float)**, so nothing in the host
userland can satisfy `rbp`. We build the RX3 **soft-float glibc 2.13** userland
and `chroot` into it, exactly like PrimeBox §3 but at
`/home/user/rbx3-run`.

Directory contents (adapted from PrimeBox `docs/02-hardware.md` §3):

```
/home/user/rbx3-run/
├── lib/                   RX3 glibc 2.13 + ld-linux.so.3 (soft-float)
├── usr/lib/               libstdc++, DirectFB 1.4, freetype, libg2d, …
├── usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so   ← rebuilt (see §2)
├── usr/bin/edb_streamd usr/bin/kill_daemon
├── root/pdj/rbp           ← rbp-audio
├── root/gui/              fonts + pset + imagedata
├── usr/share/alsa/
├── bin/sh -> busybox
├── media/usb1/            bind-mount target for the library stick
├── dev/ proc/ sys/ tmp/   bind-mounted from the host
└── etc/mtab -> /proc/mounts
```

Build the tarball on the workstation, ship it, and unpack:

```sh
WORKSTATION$ cd $RBX3
WORKSTATION$ tar czf /tmp/rbx3-run.tar.gz -C XDJRX3-rootfs lib usr bin etc/mtab
WORKSTATION$ tar czf /tmp/rbx3-extra.tar.gz -C XDJRX3 lib usr gui
WORKSTATION$ scp /tmp/rbx3-run.tar.gz /tmp/rbx3-extra.tar.gz user@chromebit:/home/user/
CHROMEBIT$ mkdir -p ~/rbx3-run && tar xzf ~/rbx3-run.tar.gz -C ~/rbx3-run
CHROMEBIT$ tar xzf ~/rbx3-extra.tar.gz -C ~/rbx3-run
CHROMEBIT$ ln -sf /proc/mounts ~/rbx3-run/etc/mtab
```

Bind mounts after every boot (needs root):

```sh
CHROMEBIT# mount --bind /dev  /home/user/rbx3-run/dev
CHROMEBIT# mount --bind /proc /home/user/rbx3-run/proc
CHROMEBIT# mount --bind /sys  /home/user/rbx3-run/sys
CHROMEBIT# mount --bind /tmp  /home/user/rbx3-run/tmp
```

## 2. Display (HDMI + DirectFB)

PrimeBox rebuilds `libdirectfb_fbdev.so` to rotate the Prime GO's portrait
panel and convert RGB565→RGB32. For the Chromebit:

1. Inspect the HDMI framebuffer and pick a mode:
   ```sh
   CHROMEBIT$ cat /sys/class/graphics/fb0/{virtual_size,bits_per_pixel}
   CHROMEBIT$ fbset -fb /dev/fb0 2>/dev/null || true
   CHROMEBIT$ modetest -M rockchip 2>/dev/null | head   # if available
   ```
   Aim for **1280×800** if the sink supports it; otherwise use 1920×1080 /
   1280×720 and scale.
2. Rebuild the fbdev module from `$PRIMEBOX/tools/build-directfb/`,
   keeping the **RGB565→RGB32 conversion**, and adjust the destination
   geometry to **fit/centre 1280×800** in the HDMI mode (no rotation).
3. Install it into the chroot:
   ```sh
   CHROMEBIT# cp libdirectfb_fbdev.so \
       /home/user/rbx3-run/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
   ```
4. Alternatively use **rb2go window mode** (`fbshim-window.c` + `rbviewer.c`):
   rb renders into a shared-memory fb and an SDL2 window shows it. On a
   console-only pmOS install there is no desktop, so the DirectFB-on-fb0 path
   is the simpler first target.

## 3. Input

There is no touchscreen and no control surface on the Chromebit.

* **USB keyboard** — adapt rb2go's `keyshim.c` (keyboard → RX3 keycodes) and
  `keyshim.so`. This is the minimum to browse the library and press LOAD/PLAY.
* **USB MIDI** — for a more DJ-like feel, use PrimeBox's `knobshim2.c`
  (MIDI CC/note → `IKeyManager::sendKey`) with any USB MIDI controller.
* Validate the keycodes against PrimeBox `primego-mapping/MAPPING.md` and
  rb2go `docs/10-keyboard-input.md`.

## 4. Audio (HDMI)

`rbp` expects the three RX3 CS4344 ALSA devices. On the Chromebit the output is
HDMI stereo:

```sh
CHROMEBIT$ aplay -l          # expect an HDMI card/PCM when a sink is connected
CHROMEBIT$ aplay -D plughw:CARD=HDMI,DEV=0 /usr/share/sounds/...  # test
```

Adapt `audioshim.c`:

* intercept `snd_pcm_open("hw:cs4344audiorev8,0" …)` → the HDMI PCM,
* negotiate stereo 44.1 kHz (S16_LE or S24_LE depending on the HDMI driver),
* fold/drop the headphone and booth channels (master → HDMI L/R),
* keep the same ALSA parameter-negotiation fixes PrimeBox already worked out
  (see the audio notes in the [PrimeBox](https://github.com/erhan-/PrimeBox)
  project).

Note: **no waveform / no playback advance without a running audio callback** —
the playhead is clocked by the ALSA period callback. Audio must be up for the
UI to animate.

## 5. USB1 library

The Chromebit USB-A is **host-only** (no gadget), so the stick is handled at the
filesystem/native-database layer — the same way the XDJ-RX3 handles its USB1
slot — not with a USB mass-storage gadget and not by emulating a folder.

**Done** — see [11-usb](11-usb.md). In short:

* `scripts/rb/device/usb-watch.sh` watches `sd*` under the dwc2 root hub
  (`/usb1/`), mounts the stick RX3-style at `/media/usb1/sda1`, binds it into
  the chroot and writes `umount`/`mount` to `/tmp/udev_usb1`;
* `rbp`'s native chain runs (`UsbMountManager` → `DbProxy::reqAttach` →
  `DbIF::mount`), DeviceSQL imports `PIONEER/rekordbox/export.pdb`, and the
  media-kind-2 detect flag becomes 2 → the UI shows **USB1 <volume label>** with
  the full library;
* the watcher re-sends the mount event until the DB is ready (a notification
  sent while `rbp` is starting up is dropped).

No `export.pdb` is required for FOLDER browsing, but a real rekordbox stick
(the tested one has 2174 songs / 160 playlists) uses the native database import.

## 6. Device stubs (`rbp` expects i.MX6 devices)

Create the same stubs PrimeBox uses so `open()` succeeds and threads don't spin
(see PrimeBox `docs/02-hardware.md` §3):

| Device | Type | Note |
|---|---|---|
| `/dev/gpiodrv` | file + read shim | `GpioManager` polls it |
| `/dev/subucom_spi{1,2}.0`, `/dev/subucom_spi_rdy{3,4}.0` | FIFOs | absent sub-MCU |
| `/dev/hidg0` | FIFO | USB HID gadget |
| `/dev/printkdrv0`, `/dev/tsc2007_2-0048` | files | ioctl-only |
| `/dev/paudiog0` | **absent** | presence makes JUCE take a dead gadget path |
| `/dev/mem` | chmod 000 | block i.MX6 register mappings |

## 7. Launcher

Adapt PrimeBox `scripts/device/start-rb.sh` + `fix-dev.sh` into a **systemd**
unit suitable for postmarketOS (`/home/user` UID 10000). rb2go
`docs/11-desktop-launcher.md` shows the pmOS/systemd pattern (`rb-window.service`,
polkit rule). For a console-only install, a simple `rb` service is enough.

## 8. Shim build toolchain (workstation)

The shims must be **soft-float EABI5, GLIBC_2.4-only** to load under the RX3
glibc 2.13:

```sh
sudo apt-get install -y gcc-arm-linux-gnueabi libc6-dev-armel-cross
RX3=…/extracted/XDJRX3-rootfs
arm-linux-gnueabi-gcc -O2 -march=armv5t -mfloat-abi=soft \
    -fno-stack-protector -fPIC -shared -o my.shim.so my.shim.c \
    -I"$RX3/usr/include" -L"$RX3/lib" -L"$RX3/usr/lib" \
    -lpthread -lc -Wl,-rpath-link,"$RX3/lib:$RX3/usr/lib"
arm-linux-gnueabi-objdump -T my.shim.so | grep GLIBC | sort -u
# only GLIBC_2.4 / GLIBC_2.7; no GLIBC_2.17/2.34, no hard-float tag
```

(The postmarketOS host compiler is hard-float and cannot produce these.)

## 9. Validation order

1. chroot + `ldd`/`rbp --help` loads (no display yet).
2. DirectFB opens `/dev/fb0` and something renders (even wrong colours/scale).
3. `rb` UI visible over HDMI.
4. Keyboard/MIDI moves the UI.
5. Audio device opens and the playhead advances.
6. USB1 appears and a track loads.
7. systemd launcher survives reboot.

See [04-roadmap](04-roadmap.md) for the concrete task list.
