# 16 — Session handoff

Written at the end of a long session that fixed playback, mixer routing and
display performance, then investigated the loud start-up **pop**. Read this
before touching the audio path. The repository is in the **working** state
(pop work reverted); one crash risk and the pop itself are still open.

## 0. TL;DR

| Subsystem | State | Doc |
|---|---|---|
| Playback (audio + middle waveform + time) | ✅ fixed this session | [15](15-playback-fix.md) |
| Mixer routing (fader 2 → deck 2) | ✅ fixed this session | [09](09-input.md) |
| Input survives `systemctl restart rb.service` | ✅ fixed this session | [13](13-keyboard.md), [15](15-playback-fix.md) |
| Display frame rate (NEON publish) | ✅ improved this session | [07](07-display-working.md) F7 |
| rb2go `getTotalLength` crash-workaround | ✅ split out in rb2go | rb2go `scripts/patch-rbp-debug.py` |
| **Loud pop when `rb` starts** | ❌ open (all three ports) | §4 |
| **`keyshim` can crash `UiMain` (repeated `openDevice`)** | ⚠️ latent, fix written then reverted | §5 |

## 1. Deployed / expected artefact hashes (working set)

| Artefact | md5 |
|---|---|
| chroot `root/pdj/rbp` (`rbp-chromebit`) | `18a64bc4d0ffd1cbd35f3a6ea447fca8` |
| `work/rb/rbp-audio` (PrimeBox patch set only) | `3706c68f7242779d46afa09f35a39acf` |
| chroot `usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so` (NEON scale, F7) | `ef8e336a4dac761bc23191c6446d6bc3` |
| chroot `usr/lib/audioshim.so` | `73b3d6d4e270133a5a6733a68b66fbbd` |
| chroot `usr/lib/keyshim.so` | `8875de779d6c048c0433e6730b25aec5` |
| chroot `usr/lib/memshim.so` | `867a19abc6174432a835bfc5e0e924f2` |
| `/home/user/start-rb.sh` | `d6b59359747d6d3602c5fb707790688f` |
| `/usr/local/bin/rbkeyd` | `c01ec148cf799abc2c9d7c165d2b6e51` |

## 2. The device right now (important)

The Chromebit was powered off mid-investigation and still has the **pop-test**
shims on its rootfs (`audioshim 9edab252…`, `keyshim e0edce7b…`). The repository
no longer contains those builds. On next power-on, redeploy the working set:

```sh
PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-module.sh
ssh root@chromebit 'systemctl restart rb.service'
```

Also note: during cleanup `kernel.core_pattern` was set to `/dev/null` at runtime
(not persistent). After a reboot it is back to `systemd-coredump`; if you crash
`rb` repeatedly, coredump compression can thrash the box — consider
`echo /dev/null > /proc/sys/kernel/core_pattern` while debugging.

## 3. Rebuild / deploy cheat-sheet

```sh
# PrimeBox checkout: https://github.com/erhan-/PrimeBox
# $RBX3 = your extracted XDJ-RX3 firmware workdir
STOCK=$RBX3/XDJRX3/pdj/rbp

# player binary (PrimeBox patch set + getPcController guard only)
STOCK=$STOCK ./scripts/rb/build-rbp.sh            # -> work/rb/rbp-chromebit

# shims (soft-float, from the RX3 rootfs)
./scripts/rb/build-shims.sh audioshim.so keyshim.so
#   audioshim: 73b3d6d4…   keyshim: 8875de77…

# DirectFB fbdev module (NEON by default, NEON=0 = scalar fallback)
./scripts/rb/rebuild-fbdev.sh                      # -> ef8e336a…
# full clean build: ./scripts/rb/build-directfb.sh

# push everything to a running Chromebit
RBP=1 PASS=<password> ./scripts/rb/deploy-module.sh
```

## 4. OPEN — loud pop when `rb` starts (all three ports)

### Evidence (Chromebit, HDMI)

Instrumented `audioshim` logging the master peak of every `writei` after a
restart:

```
writei #1   peak_m=0            (silence)
writei #17  peak_m=8388608      (0x00800000, full-scale negative in S24)
...
writei #156 peak_m=16777215     (0x00FFFFFF)
writei #501 peak_m=2            (one tiny sample)
```

So the engine emits **~200 ms of full-scale garbage** from ~23 ms to ~226 ms
after the first audio callback, before settling to silence. `audioshim` forwards
it raw, so every output (HDMI here, JP11 codec on Prime GO, TAS amp on the
phone) reproduces it as a loud pop. The duration is ~writes #17–#156 at 64
frames/period = ~9 000 frames.

### Root cause

The engine's DSP output is not clean at power-on (looks like an uninitialised
filter/settling transient). We cannot patch the engine binary, so the fix must
be in the shim/launcher.

### Fix that was written and then reverted (implement it later)

In every `audioshim.c`, arm a warm-up on `snd_pcm_prepare()` and apply a gain
ramp to the final output buffer before writing it:

```c
#define STARTUP_MUTE_FRAMES 13230UL   /* 300 ms @ 44.1 kHz */
#define STARTUP_RAMP_FRAMES  4410UL   /* 100 ms @ 44.1 kHz */
static unsigned long g_startup_pos = 0;
static int           g_startup_active = 0;

/* in snd_pcm_prepare(): g_startup_pos = 0; g_startup_active = 1; */

/* in the master writei, after building the real output buffer: */
if (g_startup_active) {
    unsigned long base = g_startup_pos, ramp_end = STARTUP_MUTE_FRAMES + STARTUP_RAMP_FRAMES;
    if (base < ramp_end)
        for (i = 0; i < size; i++) {
            unsigned long f = base + i;
            float g = (f < STARTUP_MUTE_FRAMES) ? 0.0f
                    : (f < ramp_end) ? (float)(f - STARTUP_MUTE_FRAMES) / STARTUP_RAMP_FRAMES
                    : 1.0f;
            if (g < 1.0f) scale output[i*2+0..1] by g;
        }
    g_startup_pos = base + size;
    if (g_startup_pos >= ramp_end) g_startup_active = 0;
}
```

This compiles and runs (soft-float `audioshim`, float helpers are statically
linked). 300 ms covers the observed 226 ms. It is **not deployed** — it was
reverted together with the keyshim change below after the device locked up (see
§5); the pop fix itself was never implicated in the lockup.

**Port to all three** `audioshim.c`: chromebit `scripts/rb/audioshim.c`,
PrimeBox `scripts/shims/audioshim.c`, rb2go `device/audioshim.c` (rb2go writes
to a FIFO, so scale the buffer before the `write()`).

### Quick alternative to test the hypothesis

Make the shim emit silence for the first ~300 ms unconditionally (just `memset`
the output) before adding the ramp. If the pop disappears, the ramp is the
polish.

## 5. LATENT CRASH — `keyshim` calls `openDevice()` in a tight loop

While testing, `rbp` began crash-looping before audio init (`no JuceALSA`, no
`audioshim.log`). `strace -f` on a live process showed the faulting thread:

```
20432 prctl(PR_SET_NAME, "UiMain" ...)
...
20432 --- SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x10} ---
```

i.e. the **`UiMain`** thread (started by `keyshim`) dereferences `NULL + 0x10`.
`keyshim.log` showed two `UiMain pump not running -> openDevice(0)` lines: the
startup wait loop calls `ui_pump_ensure()` up to 150 times in a tight loop, and
each call re-invokes `PanelComPeerLinux::openDevice(0)`. That function is **not
idempotent** — a second call on an already-initialised instance re-runs the
epoll/device setup and `UiMain` then hits `NULL+0x10`.

The crash is a race (more likely on a loaded system), so it is usually not hit.
Once it happens, the resulting core dumps are expensive to process and the box
can thrash to the point of being unreachable (what happened here).

### Fix to re-apply (was written, then reverted — not in git)

In `scripts/rb/keyshim.c`, change `ui_pump_ensure()` so `openDevice(0)` runs at
most once per "pump not running" episode and the two wait loops don't spin:

```c
static int ui_pump_ensure(void)
{
    static int opened = 0;                 /* RESET to 0 once running */
    void *inst;
    int n;
    if (ui_pump_running()) { opened = 0; return 1; }
    inst = *(void **)PANEL_PEER_INSTANCE_GLOBAL;
    if (!inst) return 0;
    n = *(int *)((char *)inst + PANEL_PEER_NUM_DEVICES_OFF);
    if (n <= 0) return 0;
    if (__sync_bool_compare_and_swap(&opened, 0, 1)) {   /* only the 1st caller */
        klog_str("keyshim: UiMain pump not running -> openDevice(0)\n");
        ((int (*)(void *, int))FN_OPEN_DEVICE)(inst, 0);
        if (!ui_pump_running()) {
            klog_str("keyshim: openDevice did not start it -> startThread\n");
            ((void (*)(void *, int))FN_THREAD_START)((char *)inst + JUCE_THREAD_OFF, 5);
        }
    }
    if (ui_pump_running()) { opened = 0; klog_str("keyshim: UiMain pump started\n"); return 1; }
    return 0;
}
```

and add `usleep(100000);` to both `for (int i = 0; i < 150 && !ui_pump_running(); i++)`
loops. Rebooting/redeploying `keyshim` is enough; no `rbp` rebuild needed.

## 6. Display performance (done — context for future work)

`docs/07` F7. The publish path converts 1280×800 RGB565 → 1920×1080 RGB32 on the
CPU for **every** `UpdateRegion` (rbp always dirties the full frame — verified
100% full-frame). NEON + no scratch memcpy + row cache:

| | publish | presented |
|---|---|---|
| scalar | 25.6 ms/call | ~16 fps |
| **NEON** | **16.7 ms/call** | **~19 fps** |
| publish stubbed out | 0 | ~29.5 fps (UI ceiling) |

Remaining cost is dominated by writing 8.3 MB/frame to the framebuffer
(~1.6 GB/s ⇒ ~5 ms), memory-bandwidth-bound. Beyond ~30 fps needs a lower output
mode (720p) or a VOP hardware scaler (DRM/KMS instead of fbdev).

Module compiles `-march=armv7-a -mfpu=neon -mfloat-abi=softfp`; `softfp` keeps
the base calling convention so it stays link-compatible with the soft-float
`rbp`/DirectFB. `NEON=0 ./scripts/rb/rebuild-fbdev.sh` builds the scalar version.

## 7. Gotchas hit this session (worth remembering)

* **`pkill -f "rbp -a"` in an SSH one-liner kills its own shell** whenever the
  command text also contains `/root/pdj/rbp -a` (the shell's cmdline matches).
  Kill by PID, or wrap in `[r]bp`.
* **Coredump thrash**: a crash loop + `systemd-coredump` compression can load a
  4-core box to ~10 and make it unreachable. Set `core_pattern=/dev/null` while
  debugging.
* **Do not run `rbp` by hand without `fix-dev.sh`**: `UiMain`'s `openDevice`
  opens the subucom FIFOs; without them it crashes. Use `start-rb.sh`/`rb.service`.
* **Two `rbp` instances** (e.g. a leftover strace run + the service) fight over
  the subucom FIFOs and the display; always ensure `pgrep` shows exactly one.
* **rb2go** has uncommitted changes from this session (the `patch-rbp-debug.py`
  split + docs); `device/fbshim16-phone.c` was already modified before this
  session (the user's `__NR_ioctl = 54` fix) — do not revert that.
* chromebit is **not a git repository**; `rb2go` and `PrimeBox` are. Nothing in
  chromebit or rb2go was committed.

## 8. Legal

No Pioneer/AlphaTheta firmware, no `rbp`/`rb` binary, no Denon software, no
music database and no firmware key is stored in this repository. See
`NOTICE.md`.
