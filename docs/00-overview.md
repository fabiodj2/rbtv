# 00 — Overview

## Goal

Run the **Pioneer XDJ-RX3 standalone rekordbox player** (`rbp`, internally
`rb`) on an **ASUS Chromebit CS10** running **postmarketOS edge** (armv7),
natively — no emulation — with:

* the UI on the **HDMI** output,
* **audio** out of the HDMI sink,
* **input** from a USB keyboard (and optionally a USB MIDI controller),
* the rekordbox **USB1** library from a folder / USB stick,
* a **launcher** that survives reboots.

## Why this is realistic

The XDJ-RX3 firmware ships its player as **32-bit ARM soft-float EABI5**
(glibc 2.13). The Chromebit's RK3288 kernel runs soft-float ELF natively — the
kernel does not care about the userland float ABI. This is exactly how the same
player already runs on the **Denon DJ Prime GO**, which uses the **same
Rockchip RK3288** SoC.

So the Chromebit is a *closer* target to the Prime GO than to the POCO X3:

| | POCO X3 (rb2go) | Prime GO (PrimeBox) | **rbtv (this repo)** |
|---|---|---|---|
| Kernel | aarch64 + AArch32 compat | armv7 hard-float | **armv7 hard-float** |
| SoC | Snapdragon 732G | **RK3288** | **RK3288** |
| Display | phone panel + Wayland | 800×1280 panel, DRM fb | **HDMI, DRM fb** |
| postmarketOS | yes | no (Engine OS) | **yes** |

The main work is therefore **reusing PrimeBox** (chroot + shims) and swapping
the device-specific layers:

* display: Prime GO rotates 800×1280 portrait → 1280×800; the Chromebit is
  landscape HDMI, so mostly **scale/centre** to the HDMI mode.
* audio: Prime GO maps to a 4-channel `JP11` codec; the Chromebit maps to the
  **RK3288 HDMI** stereo PCM.
* input: Prime GO uses its MIDI control surface; the Chromebit uses a
  **USB keyboard** (rb2go's `keyshim`) or a USB MIDI controller.
* init: Prime GO is Buildroot/systemd; Chromebit is **postmarketOS/systemd**
  with `/home/user` and UID 10000.

## The pieces

```
┌──────────────────────────── ASUS Chromebit CS10 ──────────────────────────┐
│  Rockchip RK3288 · HDMI out · USB-A (host) · Wi-Fi/BT · micro-USB power   │
│  postmarketOS edge (armv7, systemd)                                       │
│                                                                           │
│  ┌──────────────────── /home/user/rbx3-run (chroot) ───────────────────┐  │
│  │  soft-float glibc 2.13 + RX3 libs + DirectFB 1.4                    │  │
│  │                                                                     │  │
│  │   rbp  ── the XDJ-RX3 rekordbox player                              │  │
│  │     ▲  ▲  ▲                                                         │  │
│  │     │  │  └── keyshim.so      USB keyboard → RX3 keycodes           │  │
│  │     │  └───── audioshim.so    JUCE/ALSA → HDMI PCM (stereo)         │  │
│  │     └──────── fbshim.so       fb ioctl shim (window/panel mode)     │  │
│  │                                                                     │  │
│  │   libdirectfb_fbdev.so (rebuilt) ── scale/fit + RGB565→RGB32        │  │
│  └─────────────────────────────────────────────────────────────────────┘  │
│        ▲                 ▲                    ▲               ▲           │
│     /dev/fb0        /dev/input/*          ALSA HDMI        /tmp/udev_usb1 │
│   (HDMI mode)      (keyboard/MIDI)        (hdmi PCM)       (USB1 hotplug) │
└───────────────────────────────────────────────────────────────────────────┘
```

## What already exists

* **postmarketOS boots reliably on the Chromebit from USB** — see
  [01-postmarketos-image](01-postmarketos-image.md). SSH and Wi-Fi work.
* **PrimeBox** has a complete, working `rbp` port for the same SoC: the
  soft-float chroot recipe, the rebuilt DirectFB fbdev module, the audio/input
  shims and the launcher. All of that is source (MIT / LGPL) and can be reused.
* **rb2go** has postmarketOS-specific glue (systemd units, window mode,
  keyboard input, USB folder emulation).
* The **firmware/extraction/patching tooling** (`rx3dec`, `patch-rbp`) lives in
  the [PrimeBox](https://github.com/erhan-/PrimeBox) project.

## What is missing (the actual work)

1. Build the **soft-float RX3 chroot** for the Chromebit and ship it.
2. Build the **shims** with the soft-float cross toolchain
   (`arm-linux-gnueabi-gcc`) and validate GLIBC symbol versions.
3. Get **DirectFB** rendering on the Chromebit HDMI framebuffer.
4. Route **audio** to HDMI.
5. Wire **keyboard input** (and/or MIDI) to rb's keycodes.
6. Make the **USB1** library appear to rb.
7. Add a **systemd launcher** and test end-to-end.

Details: [03-rbp-port-plan](03-rbp-port-plan.md) and
[04-roadmap](04-roadmap.md).
