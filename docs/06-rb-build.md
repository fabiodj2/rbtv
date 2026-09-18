# 06 — Building the `rb` payload for the Chromebit

How the soft-float XDJ-RX3 `rb` runtime is built for the Chromebit (armv7
hard-float postmarketOS, HDMI framebuffer, no touchscreen).

This mirrors PrimeBox (same RK3288 SoC, framebuffer/panel mode) and uses
rb2go's postmarketOS-side device glue. The one display difference: the
Chromebit is **landscape HDMI** and needs **no rotation**, only
RGB565→RGB32 scale-to-fit.

> **Display internals are documented separately in
> [07-display-working](07-display-working.md)** — the convert/publish path,
> the `UpdateRegion` requirement, buffer 0 / no-pan, and the scale-to-fill
> change. This doc covers *building* the payload.

## Inputs

| What | Where |
|---|---|
| XDJ-RX3 v1.20 ISO tree (`pdj/rbp`, `gui/`, libs) | `$RBX3/XDJRX3` |
| XDJ-RX3 soft-float rootfs (glibc 2.13, DirectFB, ALSA) | `$RBX3/XDJRX3-rootfs` |
| Patch tools + shim sources | [PrimeBox](https://github.com/erhan-/PrimeBox) |
| postmarketOS device glue (fbshim16, keyshim, units) | `rb2go` |

`$PRIMEBOX` = a checkout of <https://github.com/erhan-/PrimeBox>;
`$RBX3` = your extracted XDJ-RX3 firmware workdir.

## Toolchain

```sh
sudo apt-get install -y gcc-arm-linux-gnueabi libc6-dev-armel-cross \
    autoconf automake libtool libtool-bin patchelf
```

All shims and modules must be **soft-float EABI5** and reference only
`GLIBC_2.4`/`GLIBC_2.7` (the RX3 chroot's glibc 2.13).

## 1. Patch the player

```sh
python3 "$PRIMEBOX/tools/patch-rbp/rbp_patch.py" \
    "$RBX3/XDJRX3/pdj/rbp" -o work/rb/rbp-audio
# stock  md5 4f2efcfc0c9e3f539289f863acfddcc6
# patched md5 3706c68f7242779d46afa09f35a39acf   (68 patches)
```

## 2. Build the shims

The PrimeBox shim Makefile is used unchanged; the RX3 rootfs is the sysroot.
The runtime rootfs has no `libc_nonshared.a` / `libpthread_nonshared.a`
(dev-only files), so the linker scripts need empty stubs:

```sh
RX3=$RBX3/XDJRX3-rootfs
mkdir -p work/rb/compat
( cd work/rb/compat && ar rcs libc_nonshared.a && ar rcs libpthread_nonshared.a )

cp -a "$PRIMEBOX/scripts/shims" work/rb/shims
make -C work/rb/shims RX3="$RX3" \
  LDFLAGS="-L$PWD/work/rb/compat -L$RX3/lib -L$RX3/usr/lib \
           -Wl,-rpath-link,$RX3/lib:$RX3/usr/lib"
make -C work/rb/shims RX3="$RX3" check      # only GLIBC_2.4 / soft-float
```

Produces `knobshim2.so`, `audioshim.so`, `fbshim-tsc.so`, `gpioshim.so`,
`tscshim.so`, `crashcatch.so`, `seqinject2`, `udplog`. (For the Chromebit,
`knobshim2` is only useful with a USB MIDI controller; keyboard input will use
rb2go's `keyshim`.)

## 3. Build DirectFB 1.4.16 (the display stack)

`rb` renders through DirectFB. The RX3's shipped DirectFB core is too old for
the patched driver, so the whole core is rebuilt soft-float.

```sh
./scripts/rb/build-directfb.sh
```

What it does, and why:

* reconstructs a soft-float sysroot from the RX3 runtime libs + the cross
  headers, and appends `fstat`/`__fdelt_chk` + `atexit` to
  `libc_nonshared.a` (`scripts/rb/compat_nonshared.c`) because glibc 2.13's
  runtime does not export them;
* clones DirectFB **1.4.16** (`origin/directfb-1.4`), applies PrimeBox's
  RK3288 diff (serialized fb ioctls, forced real fb format, FRONTONLY fallback,
  RGB565→RGB32 rotation/scale) — `directfb-full.diff`;
* applies `scripts/rb/directfb-chromebit.patch`, which enables the software
  **scale + convert** path even when rotation is off. On the Chromebit the
  logical layer is RGB565 (via the fb shim) and the real HDMI fb is RGB32, so
  conversion is always required; with `rot_deg = 0` the driver scales
  1280×800 → 1728×1080 centred in 1920×1080;
* configures `--with-gfxdrivers=none` etc. and links against the RX3 libs with
  explicit `-L` so the result carries only `GLIBC_2.4/2.7`;
* stages `work/rb/dfb/` and rewrites sonames (`libdirectfb-1.4.so.6` →
  `.so.0`, …) to match the RX3 names.

Result (`work/rb/dfb/lib/`):

```
libdirectfb-1.4.so.0.0.0   libdirect-1.4.so.0.0.0   libfusion-1.4.so.0.0.0
directfb-1.4-6/systems/libdirectfb_fbdev.so
directfb-1.4-6/wm/libdirectfbwm_default.so
directfb-1.4-6/inputdrivers/libdirectfb_{linux_input,keyboard}.so
```

> The RX3 rootfs uses module dir `directfb-1.4-0`; our rebuilt 1.4.16 core
> looks in `directfb-1.4-6`, so the chroot must provide the modules there
> (RX3's font/image *interfaces* are copied in beside ours).

## 4. Framebuffer shim (`fbshim16`)

`rb` must believe the fb is the RX3's **1280×800 RGB565**. The LD_PRELOAD shim
`fbshim16-phone.c` (rb2go) reports exactly that and silently accepts
`FBIOPUT_VSCREENINFO`. It is built `-nostdlib` (raw `svc`) so the RX3 loader
accepts it.

```
work/rb/shims/fbshim16.so   → deployed as usr/lib/fbshim.so
```

## 4b. Two driver fixes the Chromebit needs (verified on hardware)

`scripts/rb/directfb-chromebit.patch` also carries these; both are required.

1. **Publish on `UpdateRegion`, not just `FlipRegion`.** DirectFB calls
   `FlipRegion` only for full-surface swaps; `rb` redraws mostly through the
   BLIT/copy path, which calls `UpdateRegion`. With the PrimeBox driver the
   first (black) frame was published, all later frames were dropped, so the TV
   stayed black. The patch extracts `publish_rotated_frame()` and registers
   `.UpdateRegion = primaryUpdateRegion`, exactly as rb2go did (rb2go docs/09).
   Verified: `/tmp/flipdbg.log` shows `FLIP 0`, then `UPDATE 0..n`, and fb
   buffer 1 decodes to the full rekordbox UI.
2. **No `fcntl@GLIBC_2.28`.** glibc 2.28+ re-versions `fcntl`; the RX3 chroot
   ships glibc 2.13, so a modern cross build makes `dlopen()` of the module
   fail. The patch replaces the single `fcntl(fd, F_SETFD, FD_CLOEXEC)` in
   `system_initialize()` with `syscall(SYS_fcntl64, ...)`. Result carries only
   `GLIBC_2.4`/`GLIBC_2.7`.

Verified build: `work/rb/dfb/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so`,
md5 `381f11946cc32064c65a5b7ade095497`, 80124 bytes.

3. **Render into physical buffer 0 (never pan).** The RK3288 VOP does not
   present a panned fb buffer: with the CRTC on `src_y=1080` (buffer 1) the TV
   stayed black, while a write to buffer 0 showed up immediately. DirectFB
   cycles its own triple buffers, so `publish_rotated_frame()` now always
   converts into `framebuffer_base + 0` (source is still
   `rot_surface + lock->offset`) and keeps the pan at 0. `primarySetRegion()`
   was changed the same way (its `dfb_fbdev_set_mode()`/`dfb_fbdev_pan()` calls
   used to move the scanout to `lock->offset/lock->pitch`). Verify:
   `cat /sys/kernel/debug/dri/0/state` must show `src-pos=…+0+0` and
   `/sys/class/graphics/fb0/pan` must be `0,0`.
4. **Scale to fill the screen.** `fbdev_rotate_primary()` had no scaling: the
   1280x800 layer was written 1:1 into the top-left of 1920x1080. It now has a
   nearest-neighbour 16.16 fixed-point scale path for the RGB565→RGB32 case
   (`deg == 0`), so the whole destination is written (1280x800 → 1920x1080).
   Because every destination pixel is covered, stale content/letterboxing is
   cleared every frame.

Verified output:

```
FLIP  0 cfg=1280x800 spitch=2560 off=2048000 rot_deg=0 rot_en=1
UPDATE 0..n cfg=1280x800 spitch=2560 off=2048000
pan=0,0   plane src-pos=1920x1080+0+0
/dev/fb0 buffer 0 decodes to the full-screen rekordbox UI
```

## 4c. `rbp` early crash (PrimeBox patch set is not enough)

The PrimeBox `rbp-audio` build (68 patches, md5 `3706c68f…`) still SIGSEGVs on
this device ~1 s after start:

```
CRASH pc=0x0031df70 addr=0x0000009c lr=0x003921a0  r0-r3=0
```

That is `ui::IUiObjManager::getPcController()` dereferencing the uninitialised
singleton from the `NetworkMonitor` JUCE timer — the crash rb2go documents in
`docs/04`. PrimeBox does not patch it. Apply **only that guard** with our
patcher:

```sh
python3 scripts/rb/patch-rbp-crashguards.py \
    --in work/rb/rbp-audio --out work/rb/rbp-chromebit
# md5 3706c68f… -> 18a64bc4d0ffd1cbd35f3a6ea447fca8
```

With that binary `rbp` stays up (no crash log) and publishes frames.

> **Do not use rb2go's `patch-rbp-debug.py` wholesale here.** Besides the
> `getPcController` guard it also forces
> `playengine::Player::getTotalLength()` (`0x63A44`) to the "no data" sentinel,
> a phone-only workaround that leaves the deck with no duration: PLAY does
> nothing and the middle scrolling waveform never renders. See
> [15-playback-fix](15-playback-fix.md).

## 5. What still has to be assembled (next step)

* chroot: RX3 rootfs + `pdj/` + `gui/` + the DirectFB stack + `rbp-audio` +
  shims (`fbshim.so`, `audioshim.so`, keyboard shim) + `/etc/directfbrc`
  (`system=fbdev fbdev=/dev/fb0 mode=1280x800 depth=16`).
* device stubs (from `fix-dev.sh`): FIFOs/regular files for
  `subucom_spi*`, `hidg0`, `gpiodrv`, `printkdrv0`, `tsc2007_2-0048`; remove
  `paudiog0`; `chmod 000 /dev/mem`.
* no touchscreen: `tsc2007_2-0048` is just a regular file; if `rb` needs the
  touch protocol to not crash, extend `tscshim` to answer benignly (TBD).
* audio: adapt `audioshim` to the Chromebit's single HDMI PCM `hw:0,0`
  (stereo) instead of the Prime GO's 4-channel JP11 codec.
* input: rb2go `keyshim` (USB keyboard) and/or `knobshim2` (USB MIDI).
* deploy to `/home/user/rbx3-run` and run with `DFB_ROTATE=off`.
