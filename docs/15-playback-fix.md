# 15 — Playback fix: drop the `getTotalLength` workaround

**Symptom (fixed):** a track loaded and showed its title, but pressing PLAY did
nothing: no playhead movement, no sound (`peak_m=0` in `/tmp/audioshim.log`),
and the middle scrolling waveform stayed black.

**Root cause:** `scripts/rb/build-rbp.sh` applied rb2go's *whole*
`patch-rbp-debug.py`, which contains **two** phone-specific crash workarounds:

| VA | stock | patched | what |
|---|---|---|---|
| `0x31DF64`/`0x31DF68` | `movw/movt` | `mov r0,#0` / `bx lr` | `ui::IUiObjManager::getPcController()` returns NULL |
| `0x63A44` | `ldr r3,[r3,#0x38]` | `b 0x63A50` | `playengine::Player::getTotalLength()` always returns the "no data" sentinel (−259) |

The first is genuinely needed on the Chromebit (the JUCE `NetworkMonitor` timer
dereferences the uninitialised PC-controller singleton: `pc=0x0031df70
addr=0x9c`). The **second is not** — it is a phone-only workaround for a
bad-vtable call in `Ui_CycleTask`/`getTotalLength` (rb2go docs/04), and it makes
the deck permanently report *no duration*. The engine therefore never reaches
the "ready to play" state: PLAY is accepted but the transport never starts, and
the waveform that is scaled by the total length never renders.

PrimeBox's working `rbp-audio` build does **not** have that patch, which is why
the same binary plays on the Prime GO.

## The fix

Apply only the `getPcController` guard. This is done by the new
[`scripts/rb/patch-rbp-crashguards.py`](../scripts/rb/patch-rbp-crashguards.py),
and `build-rbp.sh` now uses it instead of rb2go's `patch-rbp-debug.py`:

```
stock rbp ── PrimeBox rbp_patch.py ──▶ rbp-audio      (68 patches, md5 3706c68f…)
          ── patch-rbp-crashguards.py ─▶ rbp-chromebit (2 patches, md5 18a64bc4…)
```

The `getTotalLength` workaround still exists behind
`patch-rbp-crashguards.py --with-length-quirk`, for hardware that actually
crashes at `0x63A44`.

Verification on hardware (after a track is loaded and PLAY pressed):

```
$ tail -3 /tmp/audioshim.log
audioshim: writei #160001 frames=64 written=64 peak_m=16777215 peak_hp=0
```

`peak_m` is now full-scale `S24_LE` audio instead of `0`, the playhead advances,
and the scrolling waveform fills in.

## Deploying the fix

```sh
WORKSTATION$ STOCK=$RBX3/XDJRX3/pdj/rbp \
             ./scripts/rb/build-rbp.sh                 # -> work/rb/rbp-chromebit
WORKSTATION$ RBP=1 RBPBIN=$PWD/work/rb/rbp-chromebit PASS=<password> \
             ./scripts/rb/deploy-module.sh
CHROMEBIT# systemctl restart rb.service
```

Expected: `installed rbp: 18a64bc4d0ffd1cbd35f3a6ea447fca8`.

## Related gotcha: restarting `rb.service` kills the input daemons

`start-rb.sh` recreates `/tmp/rb-keys.fifo` and `/tmp/rb-ctrl.fifo` on every
run. `rbkeyd` (keyboard) and `ddj400-bridge` keep the *old* FIFO fds open, so
after `systemctl restart rb.service` they write into a deleted inode and the
keyboard/controller appear dead even though `keyshim` is fine.

`start-rb.sh` now reopens them right after recreating the FIFOs:

```sh
systemctl try-restart rbkeyd.service
sh /home/user/ddj400-start.sh restart
```

If you ever restart the input daemons by hand, do it in this order
(player first, then the writers) or run:

```sh
CHROMEBIT# systemctl restart rb.service rbkeyd.service
CHROMEBIT# sh /home/user/ddj400-start.sh restart
```

## Files touched

| File | Change |
|---|---|
| `scripts/rb/patch-rbp-crashguards.py` | new: `getPcController` guard only (+ optional `--with-length-quirk`) |
| `scripts/rb/build-rbp.sh` | use the new patcher; expected `rbp-chromebit` md5 `18a64bc4…` |
| `scripts/rb/start-rb.sh` | reopen the FIFO writers after recreating the FIFOs |
| `scripts/rb/device/rbkeyd.c` | close the probe fd in `scan_keyboard()` (was leaking one fd per 2 s rescan) |

## Legal

No Pioneer/AlphaTheta firmware, no `rbp`/`rb` binary and no firmware key is
stored in this repository. See `NOTICE.md`.
