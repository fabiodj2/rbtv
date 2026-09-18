# 08 — Audio working: `rb` on the Chromebit HDMI output

`rbp` expects three separate XDJ-RX3 CS4344 DAC stereo devices. The Chromebit
has a single **HDMI PCM** (`card0` `VEYRON-HDMI`, `hw:0,0`, stereo). `audioshim`
maps the RX3 devices onto that PCM and downmixes.

Status: **the HDMI PCM opens, negotiates and runs; the ALSA callback drives the
engine.** Audible output needs a loaded track, which needs the USB1 library
(docs/03 §5) — that is the next subsystem.

## 1. Hardware mismatch

| | XDJ-RX3 | Chromebit |
|---|---|---|
| Outputs | 3 stereo DACs (master / headphones / booth, CS4344) | **one HDMI PCM** |
| Card names | `hw:cs4344audiorev8,0/1/2`, `hw:esaics4344audio,0` | `hw:0,0` (`VEYRON-HDMI`) |
| Channels | separate stereo devices | **2 (stereo)** |
| Rate | 44.1 / 48 kHz | 44100 / 48000 |
| Format | S24_LE, S32_LE | S16_LE, **S24_LE** |

## 2. Why the shim is needed

* `rbp` opens `hw:cs4344audiorev8,0/1/2`. Those cards don't exist, so JUCE's
  open fails and — worse — `getDeviceProperties()` cannot enumerate rates, the
  rate defaults to `0`, and `DjEngineIF::audioDeviceAboutToStart()` aborts
  (`sampleRate:0 != 44100`). It also opens the **control** interface
  `snd_ctl_open("hw:cs4344audiorev8")`.
* More fundamentally, **the playhead and waveform are clocked by the ALSA
  callback**: `PlayEngine::update()` is only called from
  `DjEngineIF::audioDeviceIOCallback()`. No running audio device ⇒ frozen
  transport even though the UI repaints.

`audioshim.so` (LD_PRELOAD) presents the RX3 devices as virtual handles and
muxes them onto the real HDMI PCM.

## 3. Chromebit adaptation

`scripts/rb/audioshim.c` (built from the PrimeBox shim) differs from the
Prime GO original in exactly the hardware-specific parts:

| | Prime GO | Chromebit |
|---|---|---|
| Real device | `hw:1,0` (JP11, 4ch) | **`hw:0,0`** (HDMI) |
| Channels | 4 | **2** |
| Downmix | n/a | 4ch RX3 mix → **stereo** (`g_out2ch`): master L/R, or headphone/cue L/R when the cue has audio |
| Format/rate | S24_LE / 44100 | S24_LE / 44100 (unchanged) |

The shim forces the real device to `S24_LE`, `2` channels, `44100 Hz` and
accepts whatever period size the kernel rounds the requested 64 frames to
(the Chromebit's `snd_soc_hdmi_codec` rounds it to **240**).

### Intercepted API

| Category | Functions |
|---|---|
| PCM | `snd_pcm_open/close`, `snd_pcm_hw_params*`, `snd_pcm_sw_params*`, `snd_pcm_prepare`, `snd_pcm_link`, `snd_pcm_writei`, `snd_pcm_readi` |
| Control | `snd_ctl_open/close/pcm_info` (fake success for the RX3 card names) |
| Scheduling | `pthread_setaffinity_np`, `sched_setaffinity`, `sched_setscheduler`, `pthread_setschedparam`, `pthread_setschedprio`, `pthread_attr_setsched*` (stubbed so RT threads can't starve the UI on Rockchip) |

First playback `snd_pcm_open` → real `hw:0,0`; subsequent ones (headphone,
booth) → virtual handles; capture → a virtual handle + silence on read.

## 4. Wiring

`start-rb.sh` loads the shims in this order (order matters — the first
definition of a symbol wins):

```
LD_PRELOAD=/usr/lib/memshim.so:/usr/lib/fbshim.so:/usr/lib/audioshim.so
```

* `memshim` — `open`/`read`/`mmap` fixups (must be first)
* `fbshim` — framebuffer ioctls
* `audioshim` — `snd_*` → HDMI

The HDMI codec also needs its routing switches on (they are, by default):

```sh
amixer -c 0 cset numid=1 on     # HDMI Jack
amixer -c 0 cset numid=6 on     # HDMI Switch
```

## 5. Build & deploy

```sh
WORKSTATION$ ./scripts/rb/build-shims.sh memshim.so audioshim.so
WORKSTATION$ RBP=1 PASS=<password> ./scripts/rb/deploy-module.sh   # module + rbp
WORKSTATION$ ssh root@chromebit 'sh /home/user/start-rb.sh'
```

`audioshim.so` md5 `632bc2301c430542e73d654cf9e33400`, only `GLIBC_2.4`.
`memshim.so` md5 `867a19abc6174432a835bfc5e0e924f2`. `deploy-module.sh` uploads
both shims, the launcher and the diagnostics.

## 6. Verify

```sh
CHROMEBIT# sh /home/user/check-audio.sh
```

Healthy output:

```
== HDMI PCM hw params ==
access: RW_INTERLEAVED
format: S24_LE
channels: 2
rate: 44100 (44100/1)
period_size: 240
buffer_size: 480

== HDMI PCM status ==
state: RUNNING
owner_pid: <rbp>

== audioshim negotiation ==
audioshim: opened real hw:0,0 for Master (mode=2->0), res=0
audioshim: real set_format(S24_LE=6) res=0
audioshim: real set_rate_near res=0 rate=44100
audioshim: real set_channels(2) res=0
audioshim: real hw_params res=0

== audioshim write counter ==   (keeps increasing, ~689/s)
11501
```

`/tmp/audioshim.log` is written by the shim (negotiation + every 500th
`snd_pcm_writei`, with peak levels for master/cue).

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `ALSA lib pcm_hw.c: … Invalid value for card` in `rbp.log` | audioshim not preloaded (rbp opens the non-existent RX3 cards) | add `/usr/lib/audioshim.so` to `PRELOAD` |
| `sampleRate:0 != 44100` abort | control-device interception missing | build the current `audioshim.c` (it fakes `snd_ctl_open` for RX3 names) |
| PCM never `RUNNING`, write counter static | `snd_pcm_open` real device failed | check `opened real hw:0,0 … res=` in `/tmp/audioshim.log`; confirm `aplay -l` shows the card |
| wrong speed / garbage | format mismatch | keep `S24_LE`; HDMI accepts S16_LE too |
| no sound but PCM RUNNING | TV mute/volume, or HDMI routing off | `amixer -c0 cset numid=6 on`, check TV input volume |
| playfield frozen | callback not running (see above) | get the PCM `RUNNING` |
| very high load | `GpioManager` polling `/dev/gpiodrv` (regular file is always poll-ready) | fixed: `memshim` parks `poll()` — see docs/07 F6 |

## Startup pop (workaround)

rb's first audio buffers contain a full-scale transient (`0x800000`, the
most-negative 24-bit value), audible as a loud pop through the speakers right
after startup. `audioshim.so` holds every output channel at zero for
`STARTUP_MUTE_MS` (default 1500 ms) after the first write, then fades in over
`STARTUP_FADE_MS` (default 300 ms). `STARTUP_MUTE_MS=0` disables it.
`/tmp/audioshim.log` logs `startup mute active` / `startup mute released`.

## 8. Files

```
scripts/rb/audioshim.c               Chromebit-adapted shim source
scripts/rb/build-shims.sh            stage + build memshim/audioshim (soft-float)
scripts/rb/memshim.c                 LD_PRELOAD device/mmap fixups
scripts/rb/start-rb.sh               LD_PRELOAD order (memshim:fbshim:audioshim)
scripts/rb/device/check-audio.sh     on-device audio diagnostic
```
