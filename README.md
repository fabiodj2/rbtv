# rbtv — rekordbox (XDJ-RX3 `rb`) on postmarketOS

Check https://www.instagram.com/p/DdOw-lLoq2x/ for Demo video.

Run the **Pioneer XDJ-RX3 standalone rekordbox player** (`rbp`, also called
`rb` internally) natively on an **ASUS Chromebit CS10** (`google-veyron-mickey`,
Rockchip RK3288) running **postmarketOS edge** (armv7, systemd), with the UI on
HDMI and audio over HDMI.

No emulation: the XDJ-RX3 firmware builds its player as 32-bit ARM
**soft-float**, and the RK3288 kernel executes it natively — exactly as already
proven on the **Denon DJ Prime GO**, which is the *same SoC family*.

## Related projects (use them, don't fork them)

| Repo | Target | What we take from it |
|---|---|---|
| [PrimeBox](https://github.com/erhan-/PrimeBox) | Denon Prime GO (RK3288) | the **working** `rbp` port: soft-float chroot, DirectFB build, shims, launcher |

The Chromebit is closer to the **Prime GO** (same SoC, hard-float kernel, no
Wayland requirement) than to the POCO X3.

## Status

| Subsystem | State | Doc |
|---|---|---|
| postmarketOS boots from USB (HDMI console, verbose logs, SSH, Wi-Fi) | ✅ done | [docs/01](docs/01-postmarketos-image.md) |
| Boots the full system from internal eMMC (no USB) | ✅ done | [docs/10](docs/10-emmc-install.md) |
| Flashable fixed image | build from upstream pmOS + `scripts/fix-image` (not shipped here) | [docs/01](docs/01-postmarketos-image.md) |
| Firmware extraction + `rbp` patching tools | ✅ in the [PrimeBox](https://github.com/erhan-/PrimeBox) project | — |
| Soft-float `rbp` chroot on the Chromebit | ✅ done | [docs/06](docs/06-rb-build.md) |
| Display (DirectFB on HDMI, 1920×1080 full screen) | ✅ done | [docs/07](docs/07-display-working.md) |
| Input (buttons via `keyshim` FIFO) | ✅ done | [docs/09](docs/09-input.md) |
| Input (DDJ-400 as control surface) | ✅ works when plugged in directly (bus-powered hub cannot feed it) | [docs/12](docs/12-ddj400.md) |
| Input (physical USB keyboard, rb2go-style) | ✅ done | [docs/13](docs/13-keyboard.md) |
| Boot autostart (player + input + USB watcher) | ✅ `rb.service` | [docs/14](docs/14-handover.md) |
| Playback with a loaded track (audio + scrolling waveform) | ✅ done | [docs/15](docs/15-playback-fix.md) |
| Audio (HDMI PCM open + engine callback) | 🔶 device/callback done | [docs/08](docs/08-audio.md) |
| USB stick + rekordbox database (USB1) | ✅ done | [docs/11](docs/11-usb.md) |

## Hardware

| | ASUS Chromebit CS10 | Pioneer XDJ-RX3 | Denon Prime GO |
|---|---|---|---|
| SoC | Rockchip **RK3288** (4× Cortex-A17) | NXP i.MX6 Quad | Rockchip **RK3288** |
| Float ABI of userland | hard-float (pmOS) | **soft-float** ARM32 | hard-float |
| Display | HDMI 1.4 | 1280×800 RGB565 | 800×1280 RGB32 |
| Touch | — | tsc2007 | ILI2117 |
| Audio | **HDMI** (RK3288 I2S) | 3× CS4344 DAC | JP11 4-ch codec |
| USB | 1× USB-A host (+ micro-USB power) | 2× host + sub-MCU | 1× USB-A host |
| RAM / storage | 2 GiB / 16 GiB eMMC | — | 2 GiB / 466 MiB root |

Full details: [docs/02-hardware.md](docs/02-hardware.md).

## Repository layout

```
README.md                 this file
NOTICE.md                 legal / trademarks / what is (not) included
LICENSE                   MIT (our code)
docs/                     findings and design documents
images/                   (not shipped) flashable postmarketOS images
scripts/
  emmc-install.sh         install the running system to the internal eMMC
  fix-image/              reproducible postmarketOS image fixes + patches
  rb/                     soft-float rbp payload: build, shims, deploy, diagnostics
    build-directfb.sh     DirectFB 1.4.16 core + Chromebit fbdev patch
    rebuild-fbdev.sh      fast rebuild of just the fbdev module
    build-rbp.sh          stock rbp -> rbp-audio -> rbp-chromebit (crash fixes)
    build-shims.sh        shims: memshim/audioshim/keyshim (soft-float)
    deploy-module.sh      push module/rbp/shims/launcher to a Chromebit
    deploy-chromebit.sh   full chroot tarball deploy
    deploy-ddj400.sh      build+install the DDJ-400 MIDI bridge on the device
    deploy-keyboard.sh    build+install rbkeyd (keyboard -> FIFO) on the device
    device/rb.service            systemd: player + USB watcher on boot
    device/rbkeyd.service        systemd: keyboard daemon
    device/ddj400-bridge.service systemd: MIDI bridge
    directfb-chromebit.patch, directfbrc, fix-dev.sh, start-rb.sh, memshim.c,
    audioshim.c, keyshim.c, shims-Makefile
    probe/                DirectFB bring-up probe (dfbtest)
    device/               on-device diagnostics + tools
                          (check-display.sh, check-audio.sh, press-key.sh,
                           usb-watch.sh, usb-probe.sh, rb-usbwatch.service,
                           ddj400-bridge.c, ddj400-start.sh,
                           ddj400-selftest.sh, rbkeyd.c, fakekbd.c,
                           quiet-console.sh)
work/                     local build scratch (gitignored)
mnt/                      loop-mount points (gitignored)
```

## Quick start

### 1. Flash postmarketOS to a USB stick

The flashable images are **not part of this repository** (large binaries).
Build one by applying [`scripts/fix-image/`](scripts/fix-image/) to the upstream
postmarketOS `google-veyron` image — see
[docs/01](docs/01-postmarketos-image.md) — then write it raw (`dd`,
balenaEtcher, …). Boot the Chromebit in developer mode and press **Ctrl+U** to
boot from USB. The fixed image boots without a keyboard, prints all logs on
HDMI, retries USB forever, and has SSH + Wi-Fi preconfigured.

Details and credentials: [docs/01-postmarketos-image.md](docs/01-postmarketos-image.md).

### 2. SSH in

```sh
ssh user@chromebit.local        # password: the postmarketOS default (change after first login)
```

### 3. Port the rekordbox player

Build and deploy the payload:

```sh
WORKSTATION$ ./scripts/rb/build-directfb.sh   # DirectFB core + patched fbdev module
WORKSTATION$ ./scripts/rb/build-shims.sh      # memshim/audioshim/keyshim (soft-float)
WORKSTATION$ ./scripts/rb/build-rbp.sh        # rbp-audio + crash fixes -> rbp-chromebit
WORKSTATION$ RBP=1 PASS=<password> ./scripts/rb/deploy-module.sh
WORKSTATION$ ssh root@chromebit 'sh /home/user/start-rb.sh'
```

How the HDMI display works (and every bug that had to be fixed):
[docs/07-display-working.md](docs/07-display-working.md). Payload building:
[docs/06-rb-build.md](docs/06-rb-build.md). Live checklist:
[docs/04-roadmap.md](docs/04-roadmap.md).

### 4. Test

Static checks (shellcheck, ARM soft-float syntax check, Python) run in CI on
every push/PR. After a deploy, verify the device with a single PASS/FAIL
smoke test:

```sh
PASS=<password> HOST=root@chromebit.local ./scripts/rb/run-device-tests.sh
```

Details: [docs/17-testing.md](docs/17-testing.md).

## Documentation

```
docs/00-overview.md          goal, architecture, why it should work
docs/01-postmarketos-image.md what was fixed to boot from USB + SSH/Wi-Fi
docs/02-hardware.md          Chromebit vs XDJ-RX3 vs Prime GO
docs/03-rbp-port-plan.md     port plan: chroot, display, input, audio, USB
docs/04-roadmap.md           ordered next steps / checklist
docs/05-chromebit-survey.md  live platform survey (display/audio/USB/input/network)
docs/06-rb-build.md          building the soft-float rbp payload (patch, shims, DirectFB)
docs/07-display-working.md   how the HDMI display works + the five fixes
docs/08-audio.md             HDMI audio: audioshim -> hw:0,0, downmix, callback
docs/09-input.md             buttons: keyshim FIFO -> IKeyManager::sendKey
docs/10-emmc-install.md      installing the whole system to the internal eMMC
docs/11-usb.md               real USB stick + native rekordbox database detection
docs/12-ddj400.md            DDJ-400 as the control surface (MIDI bridge, mapping)
docs/13-keyboard.md          physical USB keyboard control (rbkeyd, evdev -> FIFO)
docs/14-handover.md          session handover: deployed state, input, playback fix pointer
docs/15-playback-fix.md      why PLAY did nothing (getTotalLength workaround) + FIFO restart gotcha
docs/16-handoff.md           latest session handoff: hashes, rebuild/deploy, open pop + crash fixes
docs/17-testing.md           static checks (CI) + device smoke tests (selftest.sh)
scripts/fix-image/           the exact image-fix procedure + patches
scripts/rb/                  DirectFB/rbp build, deploy and on-device diagnostics
scripts/rb/probe/            DirectFB bring-up probe (dfbtest)
```

## Legal

This repository contains **no Pioneer/AlphaTheta firmware, no `rbp`/`rb`
binary, no Denon software, no music databases and no firmware decryption key.**
See [NOTICE.md](NOTICE.md).

The project's own code is **MIT** ([LICENSE](LICENSE)). Third-party files keep
their own licences: the modified postmarketOS initramfs under
[`scripts/fix-image/patched/`](scripts/fix-image/patched/) is
**GPL-2.0-or-later** (see [its README](scripts/fix-image/patched/README.md)).

*Pioneer DJ*, *AlphaTheta*, *rekordbox*, *XDJ-RX3*, *CDJ*, *Denon DJ*,
*Prime GO*, *ASUS*, *Chromebit*, *Rockchip* are trademarks of their respective
owners, used descriptively only.
