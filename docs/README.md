# Documentation

Start here:

| Doc | Contents |
|---|---|
| [00-overview](00-overview.md) | Goal, architecture, why the Chromebit can run `rbp` |
| [01-postmarketos-image](01-postmarketos-image.md) | What was fixed so postmarketOS boots from USB (verbose logs, SSH, Wi-Fi) |
| [02-hardware](02-hardware.md) | Chromebit vs XDJ-RX3 vs Prime GO, display/audio/USB/kernel facts |
| [03-rbp-port-plan](03-rbp-port-plan.md) | Subsystem-by-subsystem plan to run `rb` on the Chromebit |
| [04-roadmap](04-roadmap.md) | Ordered, actionable checklist |
| [05-chromebit-survey](05-chromebit-survey.md) | Live platform survey: CPU, storage, display, audio, USB, input, network |
| [06-rb-build](06-rb-build.md) | Building the soft-float `rb` payload (rbp patch, shims, DirectFB, fbshim) |
| [07-display-working](07-display-working.md) | How the HDMI display works + the five fixes that made `rb` visible full screen |
| [08-audio](08-audio.md) | HDMI audio: `audioshim` → `hw:0,0`, downmix, the ALSA callback |
| [09-input](09-input.md) | Buttons: `keyshim` FIFO → `IKeyManager::sendKey`, UiMain pump, keycodes |
| [10-emmc-install](10-emmc-install.md) | Booting from the internal eMMC (read-only primary GPT, `cgpt add`, signed kpart) |
| [11-usb](11-usb.md) | Real USB stick + native rekordbox `export.pdb` detection (`usb-watch.sh`) |
| [12-ddj400](12-ddj400.md) | DDJ-400 as the control surface: MIDI bridge → `/tmp/rb-ctrl.fifo` → `keyshim` → `sendKey` |
| [13-keyboard](13-keyboard.md) | Physical USB keyboard control: `rbkeyd` (evdev → FIFO), key map, mixer layer |
| [14-handover](14-handover.md) | Session handover: deployed artefacts/hashes, restore commands, input, playback fix |
| [15-playback-fix](15-playback-fix.md) | Why PLAY did nothing: the rb2go `getTotalLength` workaround, and the FIFO/daemon restart gotcha |
| [16-handoff](16-handoff.md) | **Session handoff**: deployed hashes, rebuild/deploy, open audio-pop fix, latent `keyshim`/`UiMain` crash |
| [17-testing](17-testing.md) | Static checks (CI) + device smoke tests (`selftest.sh`) |
| [18-orangepi4-lts-port-plan](18-orangepi4-lts-port-plan.md) | **Second target**: Orange Pi 4 LTS (RK3399) + USB touchscreen + DDJ-400 — plan, what carries over, what's new, survey checklist |

Related, external to this folder:

* [PrimeBox](https://github.com/erhan-/PrimeBox) — the finished Prime GO port
  (same SoC family). The best reference for every subsystem.
* `rb2go` — the finished POCO X3 / postmarketOS port.

Conventions used throughout:

* `WORKSTATION$` — commands on your PC / WSL.
* `CHROMEBIT#` — commands on the Chromebit over SSH.
* `$REPO` — this repository's path.
* `$PRIMEBOX` — a checkout of <https://github.com/erhan-/PrimeBox>.
* `$RBX3` — your extracted XDJ-RX3 firmware workdir.
