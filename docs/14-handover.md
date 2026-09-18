# 14 — Handover: DDJ-400 + keyboard input, and the open playback issue

Snapshot of the working state so this can be picked up later without re-deriving
anything.  Written after the session that added keyboard control, fixed the
DDJ-400 USB problem and left **playback/audio** open on purpose.

## 1. What works right now

| Subsystem | State | Doc |
|---|---|---|
| postmarketOS boots from eMMC, HDMI display (DirectFB) full screen | ✅ | docs/07 |
| HDMI audio (TV) — PCM RUNNING, ALSA callback drives the engine | ✅ | docs/08 |
| Buttons via `/tmp/rb-keys.fifo` → `keyshim` → `sendKey` | ✅ | docs/09 |
| **DDJ-400 as control surface** (direct USB connection) | ✅ | docs/12 |
| **Physical keyboard control** (Logitech K400 Plus) | ✅ | docs/13 |
| `getty`/printk/fbcon silenced so the player owns the screen | ✅ | docs/07 §8 |
| rekbordbox stick hot-plug → USB1 with the native `export.pdb` DB | ✅ | docs/11 |
| Everything auto-starts on boot (`rb.service`) | ✅ | docs/14 §4 |
| **Pressing PLAY with a loaded track: playback + scrolling waveform** | ✅ fixed | [docs/15](15-playback-fix.md) |
| ~~Pressing PLAY with a loaded track: no playback / silence~~ | ✅ fixed (see docs/15) | §6 |

Verified live at the end of the session:

```
rb.service             enabled / active
rbkeyd.service         enabled / active
ddj400-bridge.service  enabled / active
rbp pid 1118, USB1 detect=2 READY  label='My Music' songs=2174 playlists=160
uiConnectedMedia = 0x2 (bit1 USB1: set)
rbkeyd: keyboard /dev/input/event4 (Logitech K400 Plus)
audioshim: opened real hw:0,0 for Master (mode=2->0), res=0
```

## 2. Deployed artefacts (md5)

| File on the Chromebit | md5 |
|---|---|
| chroot `root/pdj/rbp` (rbp-audio + getPcController guard) | `18a64bc4d0ffd1cbd35f3a6ea447fca8` |
| chroot `usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` (NEON scale, docs/07 F7) | `ef8e336a4dac761bc23191c6446d6bc3` |
| `/usr/local/bin/rbkeyd` (keyboard → FIFOs) | `c01ec148cf799abc2c9d7c165d2b6e51` |
| `/usr/local/bin/fakekbd` (uinput test keyboard) | `33cd2782d915778f18b65ff6e73c6fef` |
| `/usr/local/bin/ddj400-bridge` (MIDI → FIFO) | `11c03056bde4d9557976f5d99ba844de` |
| chroot `usr/lib/keyshim.so` (+ `/tmp/rb-ctrl.fifo`) | `8875de779d6c048c0433e6730b25aec5` |
| chroot `usr/lib/audioshim.so` (HDMI chosen by card **name**) | `73b3d6d4e270133a5a6733a68b66fbbd` |

Units installed on the device (copies live in the repo):

```
/etc/systemd/system/rb.service              scripts/rb/device/rb.service
/etc/systemd/system/rbkeyd.service          scripts/rb/device/rbkeyd.service
/etc/systemd/system/ddj400-bridge.service   scripts/rb/device/ddj400-bridge.service
```

## 3. Restore / rebuild from the repo

```sh
# shims (soft-float, from the RX3 rootfs)
WORKSTATION$ ./scripts/rb/build-shims.sh keyshim.so audioshim.so

# host-side input tooling (built on the device, no cross-compiler needed)
WORKSTATION$ PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-keyboard.sh
WORKSTATION$ PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-ddj400.sh
WORKSTATION$ PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-module.sh
```

On the device:

```sh
CHROMEBIT# systemctl restart rb.service      # player + USB watcher
CHROMEBIT# systemctl restart rbkeyd          # keyboard daemon
CHROMEBIT# systemctl restart ddj400-bridge   # MIDI bridge
CHROMEBIT# /usr/local/bin/rbkeyd list        # keyboard map
CHROMEBIT# sh /home/user/quiet-console.sh status
CHROMEBIT# sh /home/user/usb-watch.sh status
```

Post-reboot ordering is automatic: `rb.service` → `start-rb.sh` (player, FIFOs,
quiet console, USB watcher), `rbkeyd.service` and `ddj400-bridge.service` wait
for their hardware and attach whenever it appears.

## 4. Quick reference: how to drive it

Keyboard (details and the full table: docs/13):

```
browse/select   b  ENTER  BACKSPACE  UP/DOWN  PGUP/PGDN
load           1 (deck 1)   2 (deck 2)
transport      p/o play   c/v cue   y/h sync   (deck 1 / deck 2)
deck           k master   g tempo range   [ loop in   ] loop out   \ reloop
pads           F1..F8   bank: F9 hot cue, F10 auto loop, F11 slip loop, F12 beat jump
jog            LEFT/RIGHT (SHIFT = coarse)
TAB            switch active deck
INSERT         mixer layer: w/s fader, e/d trim, r/f t/g y/h EQ, u/j filter,
               o/l crossfader, -/= tempo
```

DDJ-400: plug in directly (no hub) — the bridge picks it up by itself; mapping
table in docs/12 §3.  Scripted/one-shot: `rbkeyd send play 1`,
`rbkeyd send fader 1 900`, `rbkeyd send eqlow 2 700`, `rbkeyd send pad 3 2`.

## 5. Known-good invariants

* Both FIFOs live in `/tmp` (shared host ↔ chroot) and must stay **fixed record
  size**: 12 bytes for buttons, 24 bytes for values. A FIFO has no framing, so a
  variable record size would desynchronise `keyshim`.
* `keyshim.so` is loaded **last** in `LD_PRELOAD` (after memshim/fbshim/audioshim).
* Only one `rbp` may run: `start-rb.sh` now kills a previous instance.
* `/tmp` is on the rootfs (persistent) — stale logs/FIFOs survive a reboot;
  `start-rb.sh` cleans the ones that matter.
* The `audioshim` opens the HDMI card **by name** (`VEYRON…`/`HDMI`) because card
  numbering is not stable once a USB audio device is attached at boot.
* `keyshim` re-applies the synthetic mixer defaults, because a box with no
  physical mixer panel starts with faders at 0 (= silence).  It also pins the
  mixer input routing (`0x01149f50=01149f08`, `0x01149f54=01149f10`): without
  the sub-MCU both channels default to Player 0, so the channel-2 fader would
  control deck 1 (docs/09).

## 6. RESOLVED — play did not play (the `getTotalLength` workaround)

Symptom: the stick loaded, a track was loaded and shown, but pressing PLAY did
nothing — no playhead movement, `peak_m=0` and no middle scrolling waveform.

**Cause:** `build-rbp.sh` applied rb2go's whole `patch-rbp-debug.py`, which
forces `playengine::Player::getTotalLength()` (`0x63A44`) to the "no data"
sentinel. That is a phone-only workaround; it leaves the deck with no duration,
so the transport/analysis never becomes ready and PLAY is a no-op. The
`getPcController` guard from the same script *is* needed.

**Fix:** [`docs/15`](15-playback-fix.md) — only the `getPcController` guard is
applied now (new `scripts/rb/patch-rbp-crashguards.py`), `rbp-chromebit` md5
`18a64bc4d0ffd1cbd35f3a6ea447fca8`. Verified live: `peak_m` goes full-scale,
the playhead advances and the waveform fills in.

Also fixed here: restarting `rb.service` recreates the `/tmp/rb-*.fifo` FIFOs
and orphans the fds held by `rbkeyd`/`ddj400-bridge`, so the keyboard went dead
until those daemons were restarted; `start-rb.sh` now reopens them itself.

<details><summary>Original investigation notes (kept for history)</summary>

Symptom (user): the stick loaded, a track was loaded, but pressing PLAY does not
play; suspected audio.

Evidence gathered so far (all on the device, while `rbp` was running):

| Check | Result |
|---|---|
| HDMI PCM status | **RUNNING**, `hw_ptr`/`appl_ptr` advancing |
| `audioshim` negotiation | `S24_LE`, 44100 Hz, 2 ch, period 240 — success |
| ALSA callback | **running** — `writei #188501 frames=64 written=64` keeps counting |
| mixer output | `peak_m=0 peak_hp=0` → the engine is producing **silence** |
| re-applying mixer defaults (0x7f01 trigger into `/tmp/rb-keys.fifo`) | no change, still `peak_m=0` |
| browse caution `[0x05a191fc]` | `0` (cleared, so it is not gating the UI) |
| keys in `keyshim.log` | only `ctrl key=0000420c` (browse) — **no button events** were in the log at that moment |

So the audio **device** side is healthy: the callback runs, which per docs/08 is
what clocks the transport.  The open question is whether the deck is actually
playing (and just silent) or the transport never starts.

Next steps, cheapest first:

1. **Framebuffer "oscilloscope"** (started, inconclusive): hash `/dev/fb0` twice
   a few seconds apart — identical hashes = the UI is static = not playing;
   different = something is animating (playhead/meters) = playing but silent.
   ```sh
   CHROMEBIT# H1=$(md5sum /dev/fb0); sleep 3; H2=$(md5sum /dev/fb0); [ "$H1" = "$H2" ] && echo static || echo animating
   ```
2. **Prove the button path independently of the keyboard**:
   `sh /home/user/press-key.sh play 1` then `grep sendkey /tmp/keyshim.log`
   (needs `sendkey key=00004101`).  Do this before blaming the audio.
3. Check the mixer values actually in the engine (`K_FADER 0x501e`, `K_TRIM`,
   `K_XFADER`) — the defaults are sent synthetically and the first send can be
   lost; docs/09 §1.  Possibly read them from `/proc/<pid>/mem` (PrimeBox
   `docs/10-memory-map.md` may have addresses).
4. Consider the **fader-start / crossfader side**: `mixer_defaults` centres the
   crossfader; if only one deck is loaded and the fader assignment is off, the
   output stays silent.
5. Only then look at the audio path: HDMI sink/mute/volume on the TV, and whether
   `peak_m` becomes non-zero with a known-good playing source.
6. Note the docs/08 primitive: **the playhead and waveform are clocked by the
   ALSA callback** — if the transport is frozen, that is the first thing to
   blame; here it is demonstrably running, so the fault is elsewhere.

</details>

## 7. Ideas that came out of this session (worth keeping)

* **`fakekbd` (uinput)**: injects keystrokes with no hardware — made the whole
  keyboard chain testable, and immediately caught a real bug (evdev key codes
  are *not* alphabetical: QWERTY row 16–25, ASDF row 30–38, ZXCV row 44–50).
* **Grab-aware device selection**: probe with `EVIOCGRAB` → `EBUSY` means another
  program (keyd, on this image) owns that keyboard, so it would deliver nothing.
* **Prefer physical over virtual input devices**, and re-scan: the keyboard that
  is plugged in later must win over `keyd virtual keyboard`.
* **Several storage devices**: rank candidates and skip zero-size ones, otherwise
  a card reader with no card hides the rekordbox stick (docs/11).
* **`pkill -f "<pattern>"` in an SSH one-liner kills its own shell** when the
  command text contains the pattern — write `[r]bp -a` style (hit this twice).
* **`/tmp` survives reboot** on this image (it is on the rootfs).
* **`md5sum /dev/fb0` twice** is a poor-man's "is the UI animating" probe.

## 8. Legal

No Pioneer/AlphaTheta firmware, no `rbp`/`rb` binary, no music database and no
firmware key is stored in this repository.  See `NOTICE.md`.
