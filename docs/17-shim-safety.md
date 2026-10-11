# 17 — Shim safety rules (lessons from the AZ-MODS audit)

Source: public technical analysis of the decompiled AZ-MODS (1.1.2, re-checked
against 1.2.x), reported 10–11 Oct 2026. Upstream repo:
<https://gitlab.com/magnifiedtoasterpastry/az-mods-custom>.

**Evidence grade: medium-high.** The analysis cites concrete functions, hooks
and behaviours from decompiled code and was updated after inspecting 1.2.x. It
is *not* an audit of the original source and does not yet ship tests that
reproduce every failure. No AZ-MODS release after 1.2.2 and no public
framebuffer/FPS patch has appeared. Treat it as a checklist of failure modes,
not as proof about any specific build.

## Risks reported in AZ-MODS

* Hundreds of calls/patches at absolute EP147 addresses; 1.2.x adds an MD5 of
  the executable but does not validate each patched instruction.
* The process is patched while its threads are running (race condition).
* EP147 object pointers are stored and used later from other threads (may be
  freed or reused).
* Global hooks on `open`, `read`, `ioctl`, `lseek`, SQLite and other APIs affect
  the whole process.
* File I/O, logging, sleeps and process creation run inside EP147 threads
  (can stall UI or audio).
* SIGSEGV handlers can chain into each other → stack overflow during a crash.
* 44.1 kHz and firmware-1.30 addresses are hard-coded in many places.
* Dropbear/SSH runs at real-time priority 50, above normal EP147 threads.
* Some logs and crash dumps in `/tmp` are unbounded.
* USB code loading improved in 1.2.x; the concurrency, pointer and blocking
  issues reportedly remain.

## Rules for our shims (`keyshim`, `audioshim`, `fbshim`, `rbkeyd`, MIDI bridge)

1. **Verify before patching.** Check hash, version and the expected bytes at
   every patch site; refuse to run (and run unpatched) if anything differs.
2. **No player pointers across threads.** Never keep a pointer owned by `rbp`
   (e.g. `IKeyManager`, UiMain objects) beyond the call that received it.
3. **Hooks only enqueue.** Copy the data into a bounded queue and return
   immediately; a separate thread does the work.
4. **Keep the audio/UI threads clean.** No logging, file I/O, `sleep`,
   `fork/exec`/`system`, or heavy processing on them.
5. **Narrow interception.** Prefer wrapping the specific function/symbol over
   global libc hooks (`open`, `read`, `ioctl`, `lseek`).
6. **Bound all logs and dumps.** Size cap + rotation, in `/tmp` and elsewhere;
   disable core dumps (`core_pattern=/dev/null`) unless debugging.
7. **SSH stays at normal priority.** Never give Dropbear/sshd (or any helper) a
   real-time class.
8. **Safe signal handling.** At most one SIGSEGV handler; async-signal-safe
   only; reset to `SIG_DFL` on entry to avoid recursion.
9. **No hard-coded rate/addresses without a guard.** Derive sample rate from the
   stream; tie addresses to a verified firmware version (see rule 1).
10. **Automatic kill-switch.** Keep a path that disables all patches/shims
    (flag file, env var, or boot-loop counter) and falls back to stock `rbp`.

## Status checklist (audited — see [18-shim-audit](18-shim-audit.md))

- [x] Runtime verification of the rbp image before keyshim touches it (md5, docs/18 K1); patch-site byte checks already exist in `patch-rbp-crashguards.py`
- [ ] `keyshim` → FIFO path: confirm no stored `IKeyManager` pointer across
      threads (see latent `keyshim`/`UiMain` crash in [16-handoff](16-handoff.md))
- [x] `audioshim`: no logging/I/O in the ALSA callback (docs/18 A1); [ ] open pop still open
- [x] audioshim log capped/rotated; [ ] keyshim, `rb.service` and the MIDI bridge logs still unbounded
- [ ] Verify nothing runs with real-time priority except audio, deliberately
- [x] Automatic disable path: `KEYSHIM_DISABLE`, `/etc/rbtv/disable-*`, 3-strike safe mode in `start-rb.sh` (docs/18 K11)
