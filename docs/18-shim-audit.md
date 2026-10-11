# 18 — Audit of `keyshim` and `audioshim` (against docs/17) + OrangePi 4 LTS port notes

Scope: `scripts/rb/keyshim.c`, `scripts/rb/audioshim.c`, with `memshim.c`,
`start-rb.sh`, `shims-Makefile`, the systemd units and `patch-rbp-crashguards.py`
read for context. Static review only: nothing here was run on hardware, so
items marked **(verify)** are hypotheses with a stated test.
Severity: **H** can crash/corrupt/silence the player, **M** degrades or is
fragile, **L** hygiene.

## 1. Good already

* `patch-rbp-crashguards.py` checks the stock instruction word at each site and
  refuses on mismatch — exactly rule 1, but only at *build* time.
* keyshim stores no engine pointer: `get_key_manager()` re-walks the object graph
  on every call. Its work is split from the engine's UI thread (own threads +
  kernel FIFO as the bounded queue).
* Record accumulation in the FIFO readers handles partial reads.
* `audioshim` resolves the HDMI card by name, not by index.
* rbp is not rebooted by the shim; `rb.service` is `oneshot`, so there is no
  crash loop.

## 2. keyshim

| # | Sev | Finding | Fix |
|---|---|---|---|
| K1 | H | **No runtime binary verification (rule 1).** Blind derefs/writes at absolute addresses: `0x2685f2c`, `0x26870b0`, `0x05a191fc`, `0x01149f50/54`, calls to `0x3b6360/0x3b6674/0x366804`. The only guard is `strstr(cmdline,"rbp")` on a 127-byte buffer (matches any path containing "rbp"). A different `rbp` build ⇒ SIGSEGV of the whole player, or a silent write into the wrong variable. | In the constructor, hash `/proc/self/exe` (expect `18a64bc4…`; md5 or CRC32) and also check the first words of each called function; on mismatch log once and start **no** threads. |
| K2 | H | **`openDevice(0)` re-entered (already known, docs/16 §5).** Both wait loops call `ui_pump_ensure()` up to 150× with no sleep, and *both* threads plus every key event call it, concurrently. `openDevice` is not idempotent ⇒ `UiMain` NULL+0x10 crash. The fix in docs/16 is not in the tree. | Apply it: CAS one-shot per episode, `usleep(100000)` in the loops, and call it from **one** thread only. |
| K3 | H | **Unvalidated object/vtable.** `vt = *(void***)km; vt[2]` — `km` non-NULL is the only check; `vt` itself is never tested, and `km` may be a half-constructed object. TOCTOU between `get_key_manager()` and the call at shutdown. | Check `vt` and `fn` fall inside the known image ranges (rodata/text); stop injecting once a shutdown flag is seen. |
| K4 | M | **Two threads call `sendKey`/`mixer_defaults` concurrently** (+ UI thread). Thread-safety of the engine's queue is unknown. `rbp_checked`, `ctrl_surface_active` are plain/volatile, not atomic. | One injector thread fed by both FIFOs (poll on 2 fds), `__atomic_*` for flags. |
| K5 | M | **Logging per event, unbounded** (rule 6). `klog_*` = open+write+close per line; ctrl path logs 4 lines/event. A jog wheel is hundreds of events/s ⇒ >1000 file ops/s, MB/min growth on the persistent rootfs `/tmp`; cleaned only at the next `start-rb.sh`. | Log only state changes by default; verbose behind an env flag; size-capped (e.g. 256 KiB, one rotation); never in the hot path. |
| K6 | M | **FIFO input is untrusted.** FIFOs are `chmod 666` in world-writable `/tmp`; records are forwarded verbatim (`key, op, ch, param, f, l`). No allow-list, no range check, NaN `f` allowed. A stray writer can push arbitrary keycodes/channels into the engine. Also paths opened without `O_NOFOLLOW` as root. | Allow-list keycodes, `op∈{0,2,4,5}`, `ch∈0..2`, `param` 0..1023 / small delta, `isfinite(f)`; FIFOs in a root-owned `0660` dir (e.g. `/run/rbtv`, bind-mounted), `O_NOFOLLOW\|O_CLOEXEC`. |
| K7 | M | **No resync.** One short/garbled record misaligns the 24-byte stream forever (the docs/15 FIFO-restart gotcha can cause this). | 4-byte magic + length in each record; on bad magic, drop to the next magic. |
| K8 | M | **Threads exit silently and never recover** (`read` error ⇒ `break`; open failure after 10 s ⇒ return). Input dies with at most one log line. | Reopen with backoff; heartbeat line on state change. |
| K9 | L | Blind periodic writes into engine memory (`clear_browse_caution` every 0.5–1 s, `mixer_route` every 10 s) — masks a real USB-error state and races the audio thread. Keep, but only after K1 passes and only when the value is wrong (already done for the caution; not for routes). | Write only if `*p != desired`. |
| K10 | L | Constructor spawns threads in **every** process that inherits `LD_PRELOAD` (children of `system()`), before the rbp check. | Check identity first (K1) and return. |
| K11 | L | No kill-switch (rule 10). | `KEYSHIM_DISABLE=1` / flag file `/etc/rbtv/disable-keyshim` checked in the constructor; `start-rb.sh` retries without the shim after N early exits. |

## 3. audioshim

| # | Sev | Finding | Fix |
|---|---|---|---|
| A1 | H | **File I/O on the audio thread** (rule 4): `alog()` (open/vsnprintf/write/close) every 500th `writei`, plus first-write logs; at ~689 writes/s that is a blocking filesystem call every ~0.7 s inside the engine's real-time callback. | Atomic counters in the hot path; one low-priority logger thread; size-cap the log. |
| A2 | H | **Unsynchronised shared state.** Master, headphone and booth are separate handles, likely separate threads, all touching `g_mix4ch`, `g_out2ch`, `s_has_hp_audio`, `s_peak_*`, `g_startup_frames_done`, `g_real_playback`. `snd_pcm_close()` frees the real PCM and NULLs `g_real_playback` while another thread may be inside `real_snd_pcm_writei(g_real_playback…)` (check-then-use). `init_real_alsa()` sets `initialized=1` before filling the pointers (second thread sees NULL function pointers). | `pthread_once`; per-handle lock or ring buffer (HP → ring, master thread mixes); refcount the real PCM. |
| A3 | H | **`writei` always returns `size`** even on short write or error; after one failed retry the frames are silently dropped. If the sink disappears (HDMI unplug) every call fails *instantly*, so the engine — whose playhead is clocked by this call (docs/08) — runs un-paced at 100 % CPU. The `usleep` pacing only exists when `g_real_playback==NULL`. | Loop on partial writes, `snd_pcm_recover()`, and on persistent error sleep one period and try to reopen the device. |
| A4 | H **(verify)** | **24-bit samples are probably not sign-extended.** Logged peaks `0x00800000` and `0x00FFFFFF` (> 2^23) cannot come from sign-extended S24; they look like 24-bit two's complement in the low 3 bytes. Then (a) `peak_m=16777215` is just "some negative sample near −1", i.e. the "full-scale audio" proof in docs/15 is not proof; (b) the fade-in `(int32_t)((float)v*g)` turns negative samples into large positives while 0<g<1 ⇒ a noise burst for 300 ms right after the mute — plausible contributor to the unresolved start-up pop (docs/16 §4). | Dump 32 raw samples at low volume to confirm. If true, sign-extend (`(v<<8)>>8`) before metering/scaling and mask back. |
| A5 | M | **Sticky cue latch.** `s_has_hp_audio` is set once `s_peak_hp>100` and never cleared. The engine's start-up transient hits all outputs, so the HP stream can latch it at boot and the HDMI mix then follows the cue bus permanently. (The sample log `peak_hp=0` is reset every 500 writes, so it does not rule this out.) | **(verify)** log the latch event; make it a decaying detector (e.g. N ms of audio above threshold, cleared after M ms of silence) or select by an explicit PFL state. |
| A6 | M | **`snd_pcm_readi` writes `size*8` bytes** regardless of the real capture format; virtual `set_format/set_channels` calls are accepted but not remembered ⇒ buffer overrun if rbp captures 16-bit or >2 ch. | Store the per-handle format/channels; use `snd_pcm_frames_to_bytes` logic. |
| A7 | M | **Process-wide interposition (rule 5).** Overrides `sched_setscheduler`, `sched_setaffinity`, `pthread_setschedparam/prio`, `pthread_attr_setsched*`, `pthread_setaffinity_np` for *every* thread, returning success without doing anything; also defines its own `mmap` (duplicate of memshim's — dead code today, a trap if the order changes). | Remove `mmap` from audioshim. See §5 for why the scheduling stubs matter on RK3399. |
| A8 | M | **Open-order mapping.** 1st playback open = master, 2nd = HP, 3rd = booth, by counter; virtual closes do not decrement; real close resets the counter. A reopen/reconfigure sequence breaks the mapping. | Key by the requested device name (`cs4344audiorev8,0/1/2`) instead of order. |
| A9 | M | **Hard-coded 44.1 kHz** (`set_rate_near`, `test_rate`, mute frames) and `hw:0,0` fallback. The engine does require 44.1 kHz, but a sink that only offers 48 kHz fails with no fallback. | Try `hw:`; if the rate/format fails, fall back to `plughw:`. |
| A10 | M | `snd_ctl_open` returns the fake handle `0x12345`; any other `snd_ctl_*` call that reaches libasound with it dereferences 0x12345. `snd_ctl_pcm_info` returns 0 leaving `info` uninitialised. | Implement a minimal fake ctl object, or intercept every ctl call rbp makes (trace with `ltrace`/log). |
| A11 | L | `/tmp/audioshim.log` opened as root with `O_CREAT 0666`, symlink-followable; unbounded. | `O_NOFOLLOW\|O_CLOEXEC`, `/run/rbtv`, cap. |
| A12 | L | Start-up mute is implemented in the tree (docs/08) but docs/16 §4 still lists the pop as open/reverted. | Reconcile: read the deployed hash vs `73b3d6d4…`, then update docs. |

## 4. Adjacent findings (same rules, outside the two shims)

* `rbkeyd.service` / `ddj400-bridge.service` run with `-v` and append to
  `/tmp/*.log` forever, at `Nice=-5` (rule 6). Drop `-v`, use `LogsDirectory`/
  journald, remove the negative nice.
* `start-rb.sh` makes both FIFOs `chmod 666` (K6) and relies on `rm -f` of logs
  only at start.
* `memshim.c`: `open*`/`close`/`read`/`poll` are interposed globally (rule 5);
  the fd table `is_gpio[]/is_tsc[]` is unsynchronised and `dup()`ed fds are not
  tracked. Narrow to the paths it needs only if rbp calls them through the PLT.
* No runtime kill-switch anywhere (rule 10): add one common flag honoured by all
  shims and `start-rb.sh`.

## 5. OrangePi 4 LTS (RK3399) port notes

Facts below are from general knowledge of the board — confirm on the device
(`uname -m`, `/proc/config.gz`, `/proc/asound/cards`, `lsusb -t`, `dmesg`).

**What carries over unchanged:** the soft-float chroot, `rbp-chromebit`, and the
shim *logic*. The kernel precedent is the POCO X3 port (aarch64 kernel running
the AArch32 payload). RK3399's A72 and A53 cores both execute AArch32 at EL0,
so the kernel needs `CONFIG_COMPAT=y` (and `CONFIG_ARMV8_DEPRECATED`/SWP
emulation is **not** needed unless the binary uses `swp`; verify with the first
`SIGILL`). Syscall use in the shims (`openat`, `mmap2`, `poll`) is valid under
compat.

**What changes:**

1. **Heterogeneous cores (new risk).** `audioshim` makes every
   affinity/priority call a no-op. On RK3288 (4× A17) that was harmless; on
   RK3399 the audio callback can land on a Cortex-A53 when the UI is busy and
   underrun. Replace the blanket stubs with a policy: let the audio thread keep a
   *modest* `SCHED_FIFO` (e.g. 20, never above IRQ threads), and pin audio to the
   A72 pair (CPU 4–5 on mainline DT; check `lscpu`), UI/DirectFB to the rest.
   Keep any SSH/service at normal priority.
2. **Audio device names.** The matcher looks for `"HDMI"`/`"VEYRON"`
   (case-sensitive) and falls back to `hw:0,0`. On RK3399 `/proc/asound/cards`
   will list HDMI, possibly DP/SPDIF and the on-board ES8316 codec; `hw:0,0`
   may be the wrong one. Add `AUDIO_DEV` override plus case-insensitive match,
   and decide the master output (HDMI vs 3.5 mm analog vs DDJ-400, docs/12 §8).
   Re-check S24_LE support and the rounded period size on each sink.
3. **Display.** `libdirectfb_fbdev.so` assumes the RK3288 VOP ignores fb
   panning (publish only into buffer 0) and a 1920×1080 RGB32 fbdev. Re-verify
   on RK3399 (`/sys/kernel/debug/dri/0/state`, `fb0/pan`). The NEON path still
   applies. LPDDR4 + A72 should lift the ~19 fps ceiling; a KMS plane with the
   VOP scaler would remove the CPU scale entirely.
4. **USB.** `usb-watch.sh` matches the dwc2 root hub segment `/usb1/`
   (`ff580000.usb`). RK3399 uses xHCI/EHCI controllers with different sysfs
   paths: set `USBWATCH_SEG` per port after looking at
   `readlink -f /sys/block/sda`. The single-USB-port constraint disappears, so
   keyboard + DDJ-400 + stick can coexist without a powered hub (re-verify power).
5. **Device-name strings** still reference the Chromebit/veyron in comments and
   in `audioshim.c`; `fix-image/` and `emmc-install.sh` (ChromeOS GPT/kpart) are
   Chromebit-specific and do not apply. The OrangePi boots via U-Boot, so image
   and eMMC install are a separate workstream.
6. **Unchanged hazards:** K1 matters more, not less, if you ever mix `rbp`
   builds between boards.

## 6. Suggested order of work

1. K1 + K11 (verification and kill-switch) — everything else depends on them.
2. K2 (apply docs/16 §5) and A4 verification (5-minute sample dump).
3. A1–A3 (audio-thread hygiene, locking, honest `writei` return values).
4. K5/K6/K7, A11 (bounded logs, validated records, safer paths).
5. §5 items 1–2 on the real board; then display/USB.
