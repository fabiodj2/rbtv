# 17 — On-the-fly music analysis (RK3399 little cores)

## Goal

Use the RK3399's 4 idle Cortex-A53 cores (the big cores run `rbp` + DirectFB)
to analyse tracks from the USB1 library — BPM, approximate key, and
(optionally, off the critical path) structural/energy descriptors — without
ever risking the live playback engine.

rbp already computes BPM/key/beatgrid from the track's own ANLZ files on
load. The value here is analysis rekordbox does **not** already give:
cross-deck harmonic compatibility, energy/structure curves, and a background
BPM/key cross-check computed straight from the audio (stems or the source
file), independent of the ANLZ data.

## Hardware

RK3399: 2× Cortex-A72 ("big", ~1.8 GHz) + 4× Cortex-A53 ("little", ~1.4 GHz).
*Measured* on the unit (`cpuinfo_max_freq`): cpu0-3 = A53, cpu4-5 = A72.
RAM: 3.8 GiB + 1.9 GiB swap.

## Two tiers, not one — measured, not assumed

A single "run some DSP on the little cores" plan does not survive contact
with the hardware. Two clearly different costs were measured for a
synthetic 3-minute (180 s) 44.1 kHz stereo track:

### Tier 1 — light (aubio: beat tracking + pitch histogram)

| Test | Result |
|---|---|
| `mpg123` decode, MP3 320 kbps, 4×A53 | 1.17 s |
| `mpg123` decode, MP3 192 kbps, 4×A53 | 1.16 s |
| WAV read, cached | 0.078 s |
| WAV read, real USB pendrive (10.9 MB file) | 0.627 s (~17.4 MB/s) |
| `aubiotrack` (beat/BPM), WAV, 1×A53 | 4.85 s |
| `aubiotrack`, MP3 320k, 1×A53 | 6.86 s |
| `aubiopitch` (pitch histogram → approx. key), WAV, 1×A53 | 10.40 s |
| 4× `aubiotrack` in parallel, one per A53 | 4.78 s **total** (no slowdown) |

**Verdict: safe for production as designed.** Per-track cost (~15 s worst
case on one core) is far below the track's own 180 s, and parallelising
across the 4 A53 cores costs nothing extra — confirmed empirically, not
assumed. No memory pressure observed.

One honesty note: `aubiopitch` gives a dominant-pitch histogram, not a true
chromatic key (that needs chroma + template matching — Tier 2 territory).
Treat `key_approx` as a rough tonal-centre hint, not a KS-accurate key.

### Tier 2 — heavy (MFCC + chroma + self-similarity structure, librosa-style)

`essentia-utils` has no package for this architecture — the real
comparison point had to be a librosa-equivalent pipeline (MFCC, chroma,
beat, self-similarity/segmentation), which is a reasonable stand-in for
"many descriptors + structure" cost.

| Test | Result |
|---|---|
| Full pipeline, 1×A53, pinned | **372 s** (6 min 17 s) |
| Full pipeline, free scheduler (picked an A72 on its own) | **80 s** |
| 4 in parallel on A53 | **OOM killer fired** — 2 of 4 processes killed; `rbp` itself was a candidate victim |

**Verdict: Tier 2 must never run casually.** Three independent findings,
not one:

1. **CPU**: even on a big core, 80 s is close to half a track's length —
   tolerable only well off the critical path, never per-track-on-load.
2. **Memory, not CPU, is the real hazard.** Each process used 1.1–1.4 GiB
   RSS. The unit has 3.8 GiB RAM. Two or more concurrent heavy jobs
   (or one heavy job plus `rbp`'s own footprint under load) can push the
   kernel's OOM killer into choosing a victim — and it came within one
   unlucky selection of taking down the live player instead of the
   analysis script.
3. The scheduler already does the right thing *by itself* (parked the job
   on an A72, 4.6× faster) — which is itself a hazard: an A72 is where
   `rbp`'s own real-time audio/DirectFB thread lives. Letting Tier 2 float
   free without a memory ceiling means it can collide with the one thing
   that must never glitch.

## Design

```
USB1 mount ──► inotify watcher ──► FIFO queue (4 workers, Tier 1 only)
                                        │
                                        ▼
                              /tmp/rx3-analysis/<id>.json
                              { bpm, key_approx, analyzer, analyzed_at }

Tier 2: NEVER auto-triggered. Manual/operator-invoked only, one job at a
time, hard memory ceiling, OOM-preferred-victim, free CPU scheduling.
```

### Safeguards (non-negotiable, learned from the OOM near-miss)

* Tier 1 workers: `Nice=10`, no special memory limit needed (no memory
  pressure was observed), capped at 4 concurrent jobs (one per A53, by
  construction of the FIFO-queue trick below).
* Tier 2 job: `MemoryMax=1536M`, `OOMScoreAdjust=900` (killed first, by
  design, never by luck), `Nice=19`, no CPU pinning (let the scheduler use
  an A72 as measured), and serialised to exactly one job at a time via
  `flock`.
* The live engine service should get `OOMScoreAdjust=-900` so the kernel's
  choice is deliberate in both directions, not a coin flip.
* Tier 2 is **never** wired to the USB mount hook. It is opt-in, per track,
  triggered by the operator or a future UI action — not best-effort
  background work.

## Files

```
scripts/analysis/rx3-analyze.sh         Tier 1: one file -> BPM + approx key JSON
scripts/analysis/rx3-analyze-watch.sh   inotify watcher -> FIFO queue feeder
scripts/analysis/rx3-analyze-heavy.sh   Tier 2: manual, serialised, memory-capped
scripts/analysis/systemd/*.service      units for both tiers + the watcher
```

Deployment is on-device only for now (`/usr/local/bin` + `/etc/systemd/system`
on the Orange Pi) — see the handoff message for the exact copy/install
commands; this is not wired into `build-*.sh`/`deploy-*.sh` yet because the
RK3399 + DDJ-400 target itself is not in this repo.
