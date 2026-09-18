# 02 — Hardware & environment

## 1. The three machines

| | ASUS Chromebit CS10 (target) | Pioneer XDJ-RX3 (source) | Denon Prime GO (reference) |
|---|---|---|---|
| Codename | `google-veyron-mickey` | — | — |
| SoC | Rockchip **RK3288** (4× Cortex-A17 @1.8 GHz) | NXP i.MX6 Quad | Rockchip **RK3288** |
| GPU | Mali-T760 MP4 | Vivante GC2000 | Mali-T760 |
| Kernel ABI | armv7 **hard-float** | armv7 **soft-float** | armv7 **hard-float** |
| OS | postmarketOS edge (Alpine/musl, systemd) | BusyBox / in-house init | Buildroot 2023.02, systemd |
| Kernel | `6.12.96` (`#1-google-veyron`, pmOS) | Linux 3.0.101 | `6.1.111-inmusic` PREEMPT_RT |
| RAM | 2 GiB DDR3 | — | 2 GiB |
| Storage | 16 GiB eMMC | — | 466 MiB root + `/data` |
| Video out | **HDMI 1.4** | 1280×800 RGB565 panel | 800×1280 RGB32 panel |
| Touch | **none** (HDMI) | tsc2007 resistive | ILI2117 capacitive |
| Audio | **HDMI** (RK3288 I2S/HDMI) | 3× CS4344 DAC + ESAI ADC | JP11 4-ch codec (`hw:1,0`) |
| Controls | **none** | EUP/SUB MCUs over SPI | ALSA MIDI control surface |
| USB | **1× USB-A host** | 2× host + sub-MCU | 1× USB-A host |
| Wireless | Wi-Fi 802.11ac, BT 4.0 | — | — |

The **float ABI of the host userland does not matter** for running `rbp`: the
kernel executes soft-float EABI5 binaries natively. What matters is that the
player is given the RX3 **soft-float glibc 2.13 userland** in a chroot — exactly
as PrimeBox does.

## 2. Chromebit CS10 notes

* Full name: **ASUS Chromebit CS10**, a Chrome OS "stick PC" (HDMI dongle with
  a short cable), released 2015.
* CPU: quad-core Cortex-A17 (RK3288), GPU Mali-T760 MP4.
* Ports: 1× **USB 2.0 Type-A** (host), 1× micro-USB (power/OTG), microSD slot,
  HDMI.
* postmarketOS device package: `device-google-veyron` (generic veyron),
  kernel `linux-google-veyron`, category *community*.
* Device tree: `rk3288-veyron-mickey.dtb` (included in the generic image).
* pmOS wiki marks mickey features as "untested"; the generic veyron page
  reports screen, USB-A, Wi-Fi, Bluetooth, HDMI and audio working. Our own
  testing confirms **HDMI console, USB-A (hub + stick + keyboard) and Wi-Fi**
  all work on postmarketOS edge.
* USB: the only enabled controller in the mickey DT is `usb@ff580000`
  (`snps,dwc2`, **`dr_mode = "host"`**). `usb@ff540000` is disabled. There is
  therefore **no usable USB gadget/peripheral port** — the "USB1 stick" has to
  be emulated at the filesystem layer (as rb2go does), not with a mass-storage
  gadget.
* The primary GPT on the eMMC is read-only (Chromebook convention); a prebuilt
  image can only be flashed to external storage (USB/SD) or installed with
  `pmbootstrap install --sdcard`. The USB boot path here is the intended one.

## 3. Display

* Chromebit output is HDMI. Depending on the sink, the DRM mode is typically
  1920×1080 or 1280×720, RGB32 (XRGB8888) with a `rockchipdrmfb`-style fbdev
  emulation (`/dev/fb0`).
* The rekordbox UI is designed for a **1280×800 RGB565** framebuffer. It is
  rendered through **DirectFB 1.4 fbdev**.
* PrimeBox ships a rebuilt `libdirectfb_fbdev.so` that **rotates** a portrait
  fb and **converts** RGB565→RGB32. For the Chromebit the rotation is
  unnecessary (HDMI is landscape); the work is to **fit/centre 1280×800 into
  the HDMI mode** (or set the HDMI mode to 1280×720/1280×800 if the sink
  allows).

## 4. Audio

* RK3288 audio out on the Chromebit is over **HDMI** (I2S → HDMI transmitter).
  Expect a stereo PCM device in ALSA (`aplay -l`) once an HDMI sink is
  connected — the ELD/connection has to be present for the card to expose a
  PCM.
* `rbp` expects the RX3's three CS4344 DAC devices. A shim (adapted from
  `audioshim.c`) must map **Master** to the HDMI stereo device and either drop
  or fold the headphone/booth channels.

## 5. Input

* No built-in controls and no touchscreen → input comes from a **USB keyboard**
  (rb2go's `keyshim.c` translates viewer/keys to RX3 keycodes) and/or a **USB
  MIDI controller** (PrimeBox's `knobshim2.c` maps MIDI CC/notes to keycodes).
* The Chromebit's USB-A is host-only, so a USB **MIDI** controller is a good
  way to get faders/knobs (a DJ controller or a small MIDI controller).

## 6. Software versions observed on the fixed image

| Item | Value |
|---|---|
| OS | postmarketOS edge (Alpine edge, systemd) |
| Kernel | `6.12.96 #1-google-veyron` |
| Initramfs package | `postmarketos-initramfs 3.12.3-r1` |
| SSH | `openssh-server-pam 10.5_p1-r1` (`/usr/sbin/sshd.pam`) |
| Network | NetworkManager, `nmcli`/`nmtui` available |
| Shell | BusyBox ash, `/bin/sh` |

## 7. Disk budget

The Chromebit's root is a USB stick (default image ~750 MiB used). The RX3
soft-float chroot (glibc + libstdc++ + DirectFB + freetype + `gui/` fonts +
`rbp` + daemons) is roughly **55–60 MB** — plenty of room on a normal stick.
Keep ≥ 200 MB free for logs and updates.
