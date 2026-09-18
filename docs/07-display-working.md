# 07 — Display working: `rb` on the Chromebit HDMI output

How the XDJ-RX3 rekordbox UI gets onto the Chromebit's HDMI TV, and the exact
bugs that had to be fixed. Status: **working, full screen, 1920×1080**.

```
FLIP   0 cfg=1280x800 spitch=2560 off=2048000 rot_deg=0 rot_en=1
UPDATE 0..n cfg=1280x800 spitch=2560 off=2048000
pan=0,0    plane src-pos=1920x1080+0+0
/dev/fb0 physical buffer 0  ->  full-screen rekordbox UI
```

## 1. The stack

```
 rbp (soft-float ARM32, XDJ-RX3 binary)
   │  renders a 1280x800 RGB565 "layer"
   ▼
 DirectFB 1.4.16 core (rebuilt soft-float into directfb-1.4-6)
   │
   ├── LD_PRELOAD fbshim.so           (ioctl shim, device/fbshim16-phone.c)
   │      FBIOGET_VSCREENINFO -> 1280x800, 16 bpp, RGB565, pitch 2560
   │      FBIOPUT_VSCREENINFO -> accepted silently (no real modeset)
   │      tsc2007 ioctls      -> benign "no touch"
   │
   ├── LD_PRELOAD memshim.so          (scripts/rb/memshim.c)
   │      denies /dev/mem, redirects the broken RDR mmap, parks gpiodrv/tsc
   │
   └── systems/libdirectfb_fbdev.so   (patched; scripts/rb/directfb-chromebit.patch)
          • primary layer surface lives in system RAM (rot_surface)
          • every FlipRegion *and* UpdateRegion runs the publish path:
            RGB565 -> RGB32 convert, scale to 1920x1080, write to fb buffer 0
          • never pans: DFB_ROTATE=off, pan stays 0,0
   ▼
 /dev/fb0 = rockchipdrmfb   1920x1080, 32 bpp, virtual 1920x3240 (triple)
   │   physical buffer 0 = the buffer the VOP scans (src_y = 0)
   ▼
 Rockchip VOP0 -> dw-hdmi -> HDMI -> TV
```

No rotation is needed (the TV is landscape). The scale path is what makes the
1280×800 RX3 UI fill the HDMI frame.

## 2. The mismatch

| | XDJ-RX3 | Chromebit |
|---|---|---|
| Logical UI | 1280×800 **RGB565 (16 bpp)** | — |
| Real fb | `mxcfb`, 1280×800, RGB565, single/pannable | `rockchipdrmfb`, **1920×1080, RGB32**, virtual 1920×3240 (**triple**) |
| Rotation | none | none needed |
| Panning | works | **the VOP does not present a panned buffer** |

`rb` still asks for 1280×800 RGB565 (the `fbshim` makes DirectFB believe in it),
so two conversions have to happen in the driver: **16→32 bpp** and
**1280×800 → 1920×1080**.

## 3. The five fixes

### F1 — run the convert path even with rotation off (`rot_enable`)

The PrimeBox driver reuses one switch (`rot_deg`) for both "rotate" and
"convert+publish". The Chromebit runs `DFB_ROTATE=off` (no rotation wanted), so
`rot_deg == 0` and the whole publish block was skipped — DirectFB rendered into
its own buffers, reported `Flip ... OK`, and the HDMI framebuffer never changed.

Fix: add `rot_enable`, set it to 1 unconditionally, and gate the convert/publish
path on `rot_enable` while `rot_deg` only controls the rotation angle.

*Symptom we saw:* `/tmp/dfbdig9.log` full of `pool: lock idx=… rot=0`, fb dumps
`fb_black.raw == fb_red.raw`, live `/dev/fb0` all zero.

### F2 — publish on `UpdateRegion`, not only `FlipRegion`

DirectFB calls `FlipRegion` for full-surface swaps only. `rb` redraws mostly
through the BLIT/copy path, which calls `UpdateRegion`. PrimeBox had no
`UpdateRegion` handler, so exactly one frame (the first, black one) was ever
published: the driver's log showed `FLIP 0` then nothing.

Fix (taken from rb2go `docs/09`): extract `publish_rotated_frame(lock)` and
register `.UpdateRegion = primaryUpdateRegion` in `primaryLayerFuncs`.
`primaryFlipRegion()` and `primaryUpdateRegion()` both call it.

*Verified:* `FLIP 0`, then `UPDATE 0..n`.

### F3 — always render into physical buffer 0, and never pan

`dfb_fbdev_pan()` selects a buffer via the fb `yoffset` (`src_y`). With the
CRTC on `src_y=1080` (physical buffer 1) the TV showed **black**, even though
buffer 1 contained the UI; writing to buffer 0 and keeping the pan at 0 showed
up immediately. The RK3288 VOP here does not present the panned buffer.

Fix: `publish_rotated_frame()` always writes to `framebuffer_base + 0` (the
source is still `rot_surface + lock->offset`, since DirectFB cycles its own
triple buffers), and `primarySetRegion()` no longer calls
`dfb_fbdev_set_mode()`/`dfb_fbdev_pan()` with `lock->offset / lock->pitch`.

*Verified:* `pan=0,0` and `src-pos=1920x1080+0+0` in
`/sys/kernel/debug/dri/0/state` while the UI is on screen.

### F4 — scale 1280×800 to fill the whole 1920×1080 frame

`fbdev_rotate_primary()` had no scaling: the layer was written 1:1 into the
top-left of the 1920×1080 fb (and any previous content — e.g. a green test
pattern — stayed visible around it).

Fix: a nearest-neighbour, 16.16 fixed-point scale path for the RGB565→RGB32
(`src_bpp == 2`, `deg == 0`) case, iterating over the *destination* pixels
(1280×800 → 1920×1080). Because every destination pixel is written, stale
content/letterboxing is cleared each frame. ≈2M pixels/frame — the dominant
cost of the whole stack until F7 made the conversion NEON.

### F5 — loader/crash support (not display, but required for anything to show)

* **`fcntl@GLIBC_2.28`** — a modern cross build of `fbdev.c` references the
  re-versioned `fcntl`, which the RX3 glibc 2.13 cannot resolve, so `dlopen()`
  of the module fails. Replaced the single `fcntl(fd, F_SETFD, FD_CLOEXEC)` in
  `system_initialize()` with `syscall(SYS_fcntl64, …)`. The module must carry
  only `GLIBC_2.4` / `GLIBC_2.7`.
* **`rbp` early SIGSEGV** — PrimeBox's patch set alone crashes at
  `pc=0x0031df70 addr=0x9c` (JUCE `NetworkMonitor` timer →
  `IUiObjManager::getPcController()` on a NULL singleton). Apply **only** the
  `getPcController` guard on top of `rbp-audio` → `rbp-chromebit`
  (`scripts/rb/patch-rbp-crashguards.py`; do not use rb2go's
  `patch-rbp-debug.py` wholesale — its `getTotalLength` workaround stops
  playback, see [15-playback-fix](15-playback-fix.md)).
* **`directfbrc`** — `mode=1280x800 depth=16` must match the fbshim view, and
  `no-vt` avoids DirectFB 1.4.16's `vt_set_fb()` `fstat`/`struct stat` ABI
  stack-smash against glibc 2.13 (see `scripts/rb/directfbrc`).

### F6 — park the `GpioManager` poll spin (headroom)

`/dev/gpiodrv` is a regular-file stub, so `poll(fd, POLLIN, -1)` returns
immediately (regular files are always ready) and two `GpioManager` threads spin
at ~11k polls/s each (load ~18–20, ~60% CPU). `memshim` now intercepts `poll()`:
for gpio fds it clears `revents`, sleeps the requested timeout (or 50 ms) and
returns `0`, so the threads park (~19 wakeups/s each). Measured: load 18→2,
idle 9%→72%, the threads do only `nanosleep`. Same trick as PrimeBox
`fbshim-tsc.c` / rb2go `fbshim-window.c`.

### F7 — NEON RGB565→RGB32 + scale (frame rate)

The publish path (`fbdev_scale_565`, run on `gui_task` for every `UpdateRegion`)
converted 2.07 M destination pixels in scalar code and first copied the whole
1280×800 source into `rot_scratch` (2 MB/frame). That was the single biggest
slice of `gui_task` and capped the presented UI at ~16 fps (~25.6 ms/call).

Fix (`scripts/rb/directfb-chromebit-neon.patch`):

* expand each RGB565 source row to RGB32 with **NEON**, 8 px/iteration, and
  cache the expanded row across the ~1.35 destination rows that share it;
* read the source **directly** — the whole-buffer `memcpy` to `rot_scratch` was
  pointless in the scale path (`dst` is the fb/shm and never aliases `src`);
* keep the scalar path as a fallback (`sw > 2048`, or `__ARM_NEON` undefined).

The module is built `-march=armv7-a -mfpu=neon -mfloat-abi=softfp`. `softfp`
(not `hard`) is required: it keeps the **base calling convention** (float args
in core registers, no `Tag_ABI_VFP_args`), so the module stays link-compatible
with the soft-float DirectFB/`rbp`. NEON intrinsics are rejected under
`-mfloat-abi=soft`, so `softfp` is the only option. `rebuild-fbdev.sh` sets the
flags (`NEON=0` builds the portable scalar version); `build-directfb.sh` applies
this second patch after `directfb-chromebit.patch`.

Measured on the RK3288 (1280×800 → 1920×1080):

| build | publish | presented frames |
|---|---|---|
| scalar (with `memcpy`) | 25.6 ms/call | ~16 fps |
| **NEON (this patch)** | **16.7 ms/call** | **~19 fps** |
| publish stubbed out | 0 | ~29.5 fps (UI ceiling) |

The remaining publish cost is dominated by writing 8.3 MB to the framebuffer
per frame (~1.6 GB/s ⇒ ~5 ms). That is memory-bandwidth-bound, so splitting the
conversion across threads would win little; even with the publish removed `rbp`
presents at ~29.5 fps. If more is ever needed, the options are a lower output
mode (720p halves the destination pixels, at some softness) or a VOP-side
scaler (needs a DRM/KMS path instead of fbdev).

> A no-op publish (`CHROMEBIT_NOOP_PUBLISH` was a temporary diagnostic) still
> presented the full frame, so the dirty region is always 100% of the surface —
> region-limited publishing is not an option here (rbp redraws everything).

## 4. What we ruled out

* **Pixel clock / RK3288 HDMI "clock hacks".** The classic RK3288 black-screen
  cure (dedicate NPLL to VOP0, `rockchip,hdmi-rates-hz`, rewritten MPLL/PHY
  tables — Urja Rannikko / PrawnOS) is **not** needed here:
  `/sys/kernel/debug/clk/clk_summary` shows `dclk_vop0 = 148500000` (exact
  1080p60 pixel clock from GPLL), and simply writing buffer 0 lit the screen.
  The problem was the buffer/pan, not the mode. No resolution or framerate
  change is required.
* **Modes.** 1920×1080@60 is fine. (We tried 1080p30/720p/1024×768/800×600 while
  chasing the black screen; all were accepted by the driver, none mattered.)

## 5. Build & deploy

```sh
# one-time: full DirectFB core + module (clones DirectFB 1.4.16, applies
# PrimeBox's directfb-full.diff and scripts/rb/directfb-chromebit.patch)
WORKSTATION$ ./scripts/rb/build-directfb.sh

# iterate on the fbdev driver only (fast; NEON by default, NEON=0 = scalar)
WORKSTATION$ ./scripts/rb/rebuild-fbdev.sh

# build rbp: PrimeBox patch set + rb2go crash fixes
WORKSTATION$ ./scripts/rb/build-rbp.sh

# ship to a running Chromebit, then start
WORKSTATION$ RBP=1 PASS=<password> ./scripts/rb/deploy-module.sh
WORKSTATION$ ssh root@chromebit 'sh /home/user/start-rb.sh'
```

Expected artifacts:

| Artifact | md5 |
|---|---|
| `work/rb/dfb/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` | `ef8e336a4dac761bc23191c6446d6bc3` (NEON) / `381f11946cc32064c65a5b7ade095497` (pre-NEON) |
| `work/rb/rbp-audio` (PrimeBox patches only) | `3706c68f7242779d46afa09f35a39acf` |
| `work/rb/rbp-chromebit` (rbp-audio + getPcController guard) | `18a64bc4d0ffd1cbd35f3a6ea447fca8` |
| `work/rb/rbp-audio-debug` (old, with the `getTotalLength` workaround — no playback) | `36ea5171c85c87bab4db652acdcc3fd1` |

Chroot paths on the device:

```
/home/user/rbx3-run/root/pdj/rbp                                     <- rbp-chromebit
/home/user/rbx3-run/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
/home/user/rbx3-run/usr/lib/fbshim.so   <- device/fbshim16-phone.c (r7=54, NOT fork)
/home/user/rbx3-run/usr/lib/memshim.so  <- scripts/rb/memshim.c
```

## 6. Verify

```sh
# on the device: fb geometry, scanned buffer, publish log, nonzero buffers
CHROMEBIT# sh scripts/rb/device/check-display.sh   # (copied to /home/user/)
```

A healthy run:

* `/sys/class/graphics/fb0/pan` = `0,0`
* `/sys/kernel/debug/dri/0/state` plane `src-pos=1920.000000x1080.000000+0.000000+0.000000`
* `/tmp/flipdbg.log` has `FLIP` and repeated `UPDATE` lines
* physical buffer 0 is the non-empty one (`dd if=/dev/fb0 bs=8294400 count=1`)

To eyeball the frame without the TV, dump buffer 0 and decode it as
BGRA/1920×1080:

```sh
CHROMEBIT# dd if=/dev/fb0 of=/tmp/fb.raw bs=8294400 count=1
WORKSTATION$ python3 -c "from PIL import Image;d=open('/tmp/fb.raw','rb').read();Image.frombuffer('RGBA',(1920,1080),d,'raw','BGRA',0,1).convert('RGB').save('fb.png')"
```

Other checks:

```sh
# module must only need glibc 2.4/2.7
arm-linux-gnueabi-objdump -T libdirectfb_fbdev.so | grep -o 'GLIBC_[0-9.]*' | sort -u
# directfbbrc inside the chroot must contain: system=fbdev, fbdev=/dev/fb0,
#   mode=1280x800, depth=16, module-dir=/usr/lib/directfb-1.4-6, no-vt
```

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| TV black, driver logs `rotation 0 enabled` but no `FLIP`/`UPDATE` | convert path gated on `rot_deg` (F1) | deploy the patched module; keep `DFB_ROTATE=off` |
| One `FLIP`, no `UPDATE`, TV stuck on the first (black) frame | no `UpdateRegion` handler (F2) | deploy the patched module |
| TV black while `/dev/fb0` buffer 1 holds the UI, `src-pos …+1080` | VOP does not present the panned buffer (F3) | must publish to buffer 0 and keep `pan=0` |
| UI visible but only in the top-left, junk around it | no scaling (F4) | deploy the scale patch |
| `dlopen` of the module fails / `GLIBC_2.28 not found` | versioned `fcntl` (F5) | `syscall(SYS_fcntl64, …)` |
| `rbp` exits ~1 s after start, `CRASH pc=0x31df70 addr=0x9c` | missing getPcController guard (F5) | use `rbp-chromebit` (rbp-audio + rb2go debug patches) |
| UI sideways | `DFB_ROTATE` set | use `DFB_ROTATE=off` on the Chromebit |
| waveform scrolls at ~16 fps (sluggish) | scalar publish path | deploy the NEON module (F7); `NEON=0 ./scripts/rb/rebuild-fbdev.sh` reproduces the slow build |

## 8. Keeping `rbp` in the foreground (no text flashing over the player)

By default the Chromebit boots with `console=tty0 loglevel=8`, so **every kernel
message is painted straight onto `/dev/fb0`** — i.e. text flickering over the
DirectFB screen (USB enumeration retries are the worst offender: the DDJ-400
hub failure produced several lines per second).  `getty@tty1` was also enabled,
so a login prompt lived on that same framebuffer console.

Four layers remove it, all idempotent and persistent:

| Layer | What it does | Where |
|---|---|---|
| `kernel.printk = 1 4 1 7` | only `KERN_EMERG` reaches the console | `/etc/sysctl.d/99-rb-console.conf` |
| `ShowStatus=no` | systemd stops printing `[ OK ] Started …` lines | `/etc/systemd/system.conf.d/99-rb-quiet.conf` |
| `getty@tty1` disabled | no login prompt over the player (SSH unaffected) | systemd |
| `fbcon` detached | nothing can overdraw DirectFB at all | `/sys/class/vtconsole/vtcon1/bind` |

Boot messages up to the moment the player starts **are still shown** — exactly
what you want when diagnosing a boot problem — and the screen belongs to `rbp`
from then on.

```sh
CHROMEBIT# sh /home/user/quiet-console.sh status     # what is set now
CHROMEBIT# sh /home/user/quiet-console.sh apply      # (re)apply
CHROMEBIT# sh /home/user/quiet-console.sh revert     # get the console back
```

`start-rb.sh` performs the last layer while launching the player:

```sh
QUIET_CONSOLE=2   # default: silence kernel messages + detach fbcon
QUIET_CONSOLE=1   # silence kernel messages only
QUIET_CONSOLE=0   # keep the console (bring-up / debugging)
```

To re-attach the console temporarily without rebooting:

```sh
CHROMEBIT# echo 1 > /sys/class/vtconsole/vtcon1/bind
```

## 9. Where the code lives

```
scripts/rb/directfb-chromebit.patch   the driver changes (19 hunks, applies on
                                      top of PrimeBox directfb-full.diff)
scripts/rb/directfb-chromebit-neon.patch  NEON RGB565->RGB32 + scale (F7)
scripts/rb/rebuild-fbdev.sh           fast module rebuild + soname/GLIBC checks
scripts/rb/build-directfb.sh          full core + module build
scripts/rb/build-rbp.sh               stock -> rbp-audio -> rbp-chromebit
scripts/rb/deploy-module.sh           push module/rbp to a running Chromebit
scripts/rb/directfbrc                 chroot DirectFB config (mode/depth/no-vt)
scripts/rb/memshim.c                  LD_PRELOAD device/mmap fixups
scripts/rb/fix-dev.sh, start-rb.sh    device glue
scripts/rb/probe/dfbtest.c            DirectFB bring-up probe (isolates the stack)
scripts/rb/probe/build-dfbtest.sh     build the probe
scripts/rb/device/check-display.sh    on-device display diagnostic
scripts/rb/device/quiet-console.sh    silence printk/getty/fbcon over the player
```

`device/fbshim16-phone.c` (the fb ioctl shim) is the rb2go copy, rebuilt here
and deployed as `usr/lib/fbshim.so`.
