# 20 — Waveform flicker fix (H1): pitch mismatch, and how to verify it visually

**Symptom (fix applied, pending physical confirmation on hardware):** the
scrolling waveform flickers/tears, most noticeably while zooming (rotating
the jog platter changes the waveform's time-scale).

**Root cause:** `scripts/rb/start-rk3399-ddj400.sh` requested
`DFBARGS ... pixelformat=ARGB`, but most redraws on this panel path actually
negotiate a 16-bit `RGB16` surface. When the two disagree, `primaryFlipRegion`
/ `primaryUpdateRegion` (fbdev DirectFB module) alternate between an ARGB row
pitch (bytes/line) and an RGB16 row pitch for what should be the *same*
surface — the classic symptom of that mismatch is a torn/garbled frame, i.e.
flicker.

Confirmed via `work/runtime/rx3/tmp/flipdbg.log` inside the runtime chroot
(fbdev.c logs the first 8 `FLIP`/`UPDATE` calls of every process that attaches
to the display):

```
FLIP 0    cfg=1280x800 spitch=5120 off=4096000   <- ARGB pitch (1280 * 4 bytes)
UPDATE 2  cfg=1280x800 spitch=2560 off=2048000   <- RGB16 pitch (1280 * 2 bytes), same surface
```

**The fix:** force `pixelformat=RGB16` at startup so every surface negotiates
the same format from its first call, eliminating the mismatch.

## Files touched

| File | Change |
|---|---|
| `scripts/rb/start-rk3399-ddj400.sh` | `DFBARGS` pixelformat `ARGB` → `RGB16` |

Status: applied to the working tree, **not committed yet** — pending the
visual check below.

## Why this needs a physical check

Row pitch is negotiated once per surface and does not change in response to
user input — zoom only changes *which* region of the waveform is redrawn, not
the pixel format. So the fix should hold during zoom by construction. But the
log only proves the format stops disagreeing *at process attach time*; it
can't observe what happens on the physical panel while actually zooming.
Only eyes on the hardware close that gap.

## How to verify

1. **Confirm the fix is live:**

   ```sh
   grep DFBARGS scripts/rb/start-rk3399-ddj400.sh
   # expect: ...pixelformat=RGB16...
   ```

   If it still says `ARGB`, the fix isn't deployed — stop and re-apply before
   testing.

2. **Clean restart** (always test from a known-good state, not a stale
   process):

   ```sh
   sudo systemctl restart rx3-rk3399-ddj400.service
   sleep 5
   sudo systemctl is-active rx3-rk3399-ddj400.service   # expect: active
   ```

3. **Load a track and start playback.** The scrolling waveform must be
   actively animating — a static/paused waveform doesn't exercise the redraw
   path being tested.

4. **Rotate the jog wheel (platter)** on the DDJ-400 — this drives the
   waveform zoom (`K_JOG_ROT`, see `docs/12-ddj400.md`). Do a slow turn first,
   then a few fast full turns, in both directions. Repeat 4-5 times to rule
   out an intermittent artifact.

5. **Watch the waveform during the zoom transition itself**, not just
   before/after — the flicker being tested for is a transient during the
   scale change, not a static, always-on artifact.

## Pass / fail

| Observation | Verdict | Next step |
|---|---|---|
| No visible flicker/tearing during any zoom rotation | **Fix confirmed** | Commit it: `git add scripts/rb/start-rk3399-ddj400.sh && git commit` |
| Flicker still visible during zoom | **Fix incomplete** | Do not commit. Re-open the investigation — capture a fresh flip log and journal right after reproducing (see below), and check whether a *different* code path drives the zoom redraw. |
| Flicker only when a track *loads*, not during zoom itself | **Different bug** | Likely unrelated to H1 — note it separately, don't block the H1 decision on it. |

## If it still flickers: capture evidence before touching anything

```sh
cp work/runtime/rx3/tmp/flipdbg.log ~/rx3-private/flipdbg.log.zoom-repro-$(date +%Y%m%d-%H%M%S)
sudo journalctl -u rx3-rk3399-ddj400.service --since "-2min" --no-pager \
  > ~/rx3-private/journal.zoom-repro-$(date +%Y%m%d-%H%M%S).log
```

Do this immediately after reproducing, before restarting anything — the flip
log only keeps the first 8 calls per process (`static int` counters in
`fbdev.c`) and stops recording after that, so a stale restart throws away the
evidence.

## Related

* `docs/07-display-working.md` — how the display pipeline (fbdev, scale,
  publish) works in general.
* `docs/12-ddj400.md` — DDJ-400 MIDI mapping, including jog platter rotation.
