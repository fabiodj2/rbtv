# 18 — Orange Pi 4 LTS (RK3399) + touchscreen + DDJ-400: port plan

Second hardware target, alongside the Chromebit. Status: **plan only** — no
survey done yet, nothing deployed. This doc exists so the next session (or
you, pasting command output back) can go straight to Phase 0 instead of
re-deriving the architecture question.

## 0. Why this is a different port, not just "another box"

The Chromebit (RK3288) port works because the RK3288 kernel is **32-bit
armv7**, and the XDJ-RX3's `rbp` binary is **32-bit ARM soft-float EABI5** —
same word size, same instruction set, just a different float ABI and libc
version, solved with a soft-float chroot (see [docs/02](02-hardware.md) §1
and [docs/06](06-rb-build.md)).

The RK3399 is a **big.LITTLE 64-bit** SoC (2× Cortex-A72 + 4× Cortex-A53).
Every mainstream OS for it (postmarketOS, Armbian, Debian) runs an
**aarch64** kernel and userland. `rbp` is still the same 32-bit ARM binary, so
it needs the kernel's **32-bit compat mode** (`CONFIG_COMPAT` /
`CONFIG_ARM64_COMPAT`, i.e. "AArch32 execution state") to run at all — the
same situation **rb2go** already solved on the POCO X3 (Snapdragon 732G,
aarch64 + AArch32 compat), per the comparison table in
[docs/00-overview](00-overview.md):

| | POCO X3 (rb2go) | Prime GO (PrimeBox) | Chromebit (rbtv) | **Orange Pi 4 LTS (this doc)** |
|---|---|---|---|---|
| Kernel | **aarch64 + AArch32 compat** | armv7 hard-float | armv7 hard-float | **aarch64 + AArch32 compat (assumed)** |
| SoC | Snapdragon 732G | RK3288 | RK3288 | **RK3399** |
| Display | phone panel + Wayland | 800×1280 panel, DRM fb | HDMI, DRM fb | **HDMI + touch panel?, DRM (KMS or fbdev?)** |
| Touch | capacitive (phone) | ILI2117 capacitive | — | **USB HID touchscreen** |
| postmarketOS | yes | no (Engine OS) | yes | **unknown — you said an OS is already installed** |

So for the **kernel/chroot/shim-build side, rb2go is the template**, not
PrimeBox — this repo already carries rb2go-derived code
(`scripts/rb/keyshim.c`, `scripts/rb/device/rbkeyd.c`, `fbshim16-phone.c`) so
that groundwork isn't starting from zero. For the **touch side, PrimeBox is
the template** (Prime GO has a capacitive touch panel and ships
`fbshim-tsc.c` — see §4 below), which we haven't needed until now.

## 1. What almost certainly carries over unchanged

* **The chroot/build methodology.** `scripts/rb/build-shims.sh`,
  `shims-Makefile`, `build-rbp.sh`, `patch-rbp-crashguards.py`: none of it is
  Chromebit-specific. They cross-compile with `arm-linux-gnueabi-gcc
  -march=armv5t -mfloat-abi=soft` against the RX3 rootfs as sysroot — the
  **host** architecture (armv7 hard-float vs aarch64) doesn't change any of
  that, it's cross-compilation either way.
* **`rbp`'s internal addresses.** Every port (PrimeBox, rb2go, rbtv) runs the
  **identical XDJ-RX3 v1.20 binary** — the `IKeyManager::sendKey` addresses in
  `keyshim.c`, the USB1 detection offsets in
  [`usb-probe.sh`](../scripts/rb/device/usb-probe.sh), the `getPcController`
  crash-guard: all SoC-independent. Nothing there needs re-deriving.
* **The DDJ-400 bridge.** [`ddj400-bridge.c`](../scripts/rb/device/ddj400-bridge.c)
  runs on the **host** rootfs (not the soft-float chroot), has no `libasound`
  dependency (reads `/dev/snd/midiC*D*` raw and parses MIDI itself), and only
  needs the device's own `gcc` — see [docs/12](12-ddj400.md) §1. It should
  build and run on the Orange Pi unmodified. This is the lowest-risk piece of
  the whole port.
* **`keyshim`'s keycode tables and the mixer-default logic** — SoC-independent
  for the same reason as above.

## 2. What has to be re-derived per subsystem

### a. Kernel/compat (blocks everything else)

Confirm the installed OS actually has 32-bit compat, before anything else:

```sh
uname -a                       # expect aarch64
cat /etc/os-release
zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_COMPAT|CONFIG_ARM64_COMPAT' \
  || grep -E 'CONFIG_COMPAT|CONFIG_ARM64_COMPAT' /boot/config-$(uname -r) 2>/dev/null
ls /lib/ld-linux.so.3 /lib/ld-linux-armhf.so.3 2>/dev/null   # 32-bit loader present?
```

If there's no 32-bit loader on the rootfs, that's still fine — the RX3
chroot brings its **own** soft-float `ld-linux.so.3` (same as the Chromebit,
see [docs/06](06-rb-build.md)); what must exist is kernel-side `CONFIG_COMPAT`
support to execute a 32-bit ELF at all. Most distro RK3399 kernels (Armbian,
postmarketOS) enable this by default since it's needed for 32-bit Mali/GPU
blobs on many RK3399 boards, but confirm rather than assume.

### b. Display: DRM/KMS path + touch overlay

Unlike the Chromebit (simple HDMI, no touch), this target needs to find out:

* Does `/dev/fb0` exist (legacy fbdev emulation), or is it DRM/KMS-only?
  Modern Rockchip DRM drivers often ship **without** `CONFIG_DRM_FBDEV_EMULATION`.
  DirectFB 1.4 (what this repo uses, see [docs/07](07-display-working.md))
  wants `/dev/fb0`; if it's missing, the options are (a) enable fbdev
  emulation in the kernel config, or (b) move to DirectFB's DRM system module
  instead of `fbdev` — a bigger change, only decide once you know which case
  this is.
* Whether the touchscreen is a **separate HDMI monitor + USB touch overlay**
  (most likely, given "USB HID touch" — same class of device as a USB
  touch-monitor) or a panel wired through the Orange Pi's own DSI/LCD
  connector. You already said USB HID, which is good news: it means display
  and touch are two independent, standard Linux subsystems (DRM output +
  evdev input), not a single vendor panel driver to fight.

Survey commands (Phase 0, §5):

```sh
ls /dev/fb0 2>/dev/null && echo "fbdev: present" || echo "fbdev: ABSENT (DRM-only?)"
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
ls /sys/class/drm/
for c in /sys/class/drm/card*-*; do echo "$c: $(cat $c/status 2>/dev/null)"; done
```

### c. Audio

RK3399 boards typically expose **HDMI audio** (same `dw-hdmi`-style path as
the Chromebit — `audioshim.c` should port with just a different `hw:X,Y`) and
*may* additionally expose an onboard/header codec via the RK809 PMIC or a
board-specific codec chip, depending on how the Orange Pi 4 LTS's device tree
was built for whatever OS is on it now. Don't assume which one is wired up —
survey:

```sh
cat /proc/asound/cards
aplay -l
```

If only HDMI shows up, audio is a straight port of `audioshim.c` (map Master
to HDMI PCM, same as [docs/02](02-hardware.md) §4). If a second card appears,
decide which one you actually want (TV/monitor speakers via HDMI vs a direct
audio-out via the onboard codec) before wiring the shim.

### d. Touch input — new subsystem, PrimeBox is the template

Nothing in this repo talks to a touchscreen yet — the Chromebit has none.
But **Prime GO does** (ILI2117 capacitive panel), and PrimeBox ships
`fbshim-tsc.so` for exactly this (see the target already declared, unused
until now, in [`shims-Makefile`](../scripts/rb/shims-Makefile) `SHARED` list
and pulled in by `build-shims.sh`'s `cp -a "$PBSHIMS/." "$WORK/"` staging
step). That file is **not** in this repo — it lives in the PrimeBox checkout
(`$PRIMEBOX/scripts/shims/fbshim-tsc.c`) and is copied in at build time, same
as the other shims that came from there.

Plan, mirroring how `memshim.c`/`audioshim.c`/`keyshim.c`/`fbshim16-phone.c`
were each adapted from a PrimeBox/rb2go original into a Chromebit-specific
override checked into `scripts/rb/`:

1. Read PrimeBox's `fbshim-tsc.c` to see how it injects touch events into
   `rbp` (almost certainly a Panel/UI touch-input path analogous to
   `IKeyManager::sendKey`, since the RX3 binary itself has real touch
   hardware — see [docs/02](02-hardware.md) row "Touch" for the XDJ-RX3 —
   the injection point already exists in the binary, unlike keys which
   `keyshim` had to reverse-engineer from scratch).
2. PrimeBox's version reads an **I2C ILI2117** device. Ours needs to read a
   **generic USB HID multitouch evdev** node instead
   (`/dev/input/eventN`, `ABS_MT_POSITION_X/Y`, `BTN_TOUCH` — the same evdev
   API `rbkeyd.c` already uses for the keyboard, see
   [docs/13](13-keyboard.md)), so the input-reading half is closer to
   `rbkeyd.c` than to PrimeBox's I2C code; only the "call into rbp" half
   comes from `fbshim-tsc.c`.
3. Coordinate mapping: the touch panel's raw resolution vs the 1280×800
   canvas `rbp` renders to needs a scale/offset, same class of problem as the
   display fit/centre work in [docs/07](07-display-working.md) — find out the
   touchscreen's `ABS_X`/`ABS_Y` max values with `evtest` or
   `libinput debug-events` before writing the mapping.
4. Name it `scripts/rb/touchshim.c` (or `fbshim-tsc-usb.c` to keep the
   PrimeBox naming convention), added as a new build-shims.sh target and a
   new `SHARED` line in `shims-Makefile`, same pattern as every other shim.

This is the one genuinely new subsystem in this port and the one most likely
to need a debugging session on hardware, like display did for the Chromebit
([docs/07](07-display-working.md) documents five separate fixes it took). Budget
for that rather than expecting it to work first try.

## 3. What we still don't know (please check and report back)

This session has no network path to your LAN, so it can only work from
docs and what you paste back — same "survey first" approach as
[docs/05-chromebit-survey.md](05-chromebit-survey.md) used for the Chromebit.
Please run and paste back:

```sh
uname -a
cat /etc/os-release
cat /proc/asound/cards; aplay -l
ls /dev/fb0 2>/dev/null; cat /sys/class/graphics/fb0/virtual_size 2>/dev/null
ls /sys/class/drm/; for c in /sys/class/drm/card*-*; do echo "$c: $(cat $c/status 2>/dev/null)"; done
ls /dev/input/by-id/ /dev/input/by-path/ 2>/dev/null
cat /proc/bus/input/devices | grep -A5 -i touch
lsusb
df -h /
```

Plus: which OS is actually installed (postmarketOS / Armbian / Debian /
something else) and its kernel version — that decides whether Phase 2 below
starts from an existing 32-bit-compat-ready image or needs a kernel
config change first.

## 4. Phased roadmap (mirrors docs/04's structure)

### Phase 0 — Confirm the target environment ⏳ (blocks everything)
- [ ] OS + kernel identified, aarch64 confirmed
- [ ] 32-bit compat confirmed (or a plan to enable it)
- [ ] Display: `/dev/fb0` present? DRM connector(s) and status
- [ ] Audio: `aplay -l` output
- [ ] Touchscreen enumerates as a USB HID device; note vendor/product ID and
      `ABS_X`/`ABS_Y` ranges
- [ ] Free disk space for the ~60 MB RX3 chroot (see [docs/02](02-hardware.md) §7)

### Phase 1 — Chroot + shims (expect this to be easy — see §1)
- [ ] `build-shims.sh` run as-is against the same RX3 rootfs used for the
      Chromebit (no SoC-specific code in this step)
- [ ] Chroot deployed, `rbp` launches under `chroot`/AArch32 compat without
      immediately crashing (the existing `getPcController` guard should still
      be needed and still be enough — same binary)

### Phase 2 — Display ⏳ (depends on Phase 0 findings)
- [ ] DirectFB opens the framebuffer (fbdev path) or the DRM path is chosen
      instead
- [ ] UI renders full-screen at the sink's native resolution

### Phase 3 — Audio ⏳
- [ ] `audioshim` ported to whichever ALSA `hw:X,Y` Phase 0 found (start with
      HDMI, same as the Chromebit)

### Phase 4 — Touch input ⏳ (new subsystem — see §2d)
- [ ] PrimeBox `fbshim-tsc.c` read and understood
- [ ] USB HID evdev touch shim written, taps register in `rbp`
- [ ] Coordinate mapping correct across the whole 1280×800 canvas

### Phase 5 — DDJ-400 ⏳ (expect this to be easy — see §1)
- [ ] `ddj400-bridge` built with the device's own `gcc`, runs unmodified
- [ ] Existing MIDI mapping ([docs/12](12-ddj400.md)) verified — should need
      zero changes since it only talks to `keyshim` via the FIFO, never to
      hardware directly

### Phase 6 — Productise
- [ ] systemd (or whatever this OS uses) units for boot autostart, mirroring
      [docs/14](14-handover.md) §4
- [ ] USB1 library detection re-verified on this device

## 5. Legal note

Same as the rest of this repo: no Pioneer/AlphaTheta firmware, no `rbp`
binary and no firmware key are stored here. You need your own extracted RX3
firmware (see [docs/06](06-rb-build.md)) and your own PrimeBox checkout, same
as for the Chromebit target. See [NOTICE.md](../NOTICE.md).
