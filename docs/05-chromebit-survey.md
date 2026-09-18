# 05 — Chromebit platform survey

Read-only survey of the live device, used to plan the `rb` port. Regenerate at
any time with [`../scripts/survey/chromebit-survey.sh`](../scripts/survey/chromebit-survey.sh):

```sh
sshpass -p "$PASS" ssh root@chromebit 'sh -s' \
    < scripts/survey/chromebit-survey.sh > survey.txt
```

Survey date: after the postmarketOS image fix, device running from the USB
stick.

## 1. Identity

| | |
|---|---|
| Hostname | `chromebit` |
| OS | postmarketOS edge (Alpine/musl, systemd) |
| Kernel | `6.12.96 #1-google-veyron` (`armv7l`) |
| Model | `Google Mickey` |
| Compatible | `google,veyron-mickey-rev8 … google,veyron-mickey, google,veyron, rockchip,rk3288` |
| Initramfs | `postmarketos-initramfs 3.12.3-r1` |

## 2. SoC / CPU / memory

* **4× ARMv7 Cortex-A17** (`CPU part 0xc0d`, rev 1), NEON/VFPv4/LPAE.
* cpufreq OPPs: 126 MHz … **1.8 GHz**.
* **2 GiB RAM** (~1.5 GiB available), **2.9 GiB zram** swap.

## 3. Storage

| Device | Size | Notes |
|---|---|---|
| `sda` (SanDisk Ultra USB stick) | 64 GB | the boot/root device |
| `sda1` | 16 MiB | ChromeOS kernel partition (`vmlinuz.kpart`) |
| `sda2` | 512 MiB ext2 | `/boot` |
| `sda3` | 57.8 GB ext4 | `/` |
| `mmcblk0` | 14.7 GB | internal eMMC (unused; ChromeOS/stock) |
| `mtdblock0` | 4 MiB | SPI flash (firmware/GBB) |

**Important:** the first boot resized the *partition* to the full stick but the
**ext4 filesystem was left at ~700 MiB** (86 MiB free). This was fixed with:

```sh
resize2fs /dev/sda3      # → 56 GiB total, 53 GiB free
```

The repo's initramfs patch also retries/resizes cleanly, but this one is worth
double-checking after any re-flash.

## 4. Display

| | |
|---|---|
| Framebuffer | `/dev/fb0` = **`rockchipdrmfb`** |
| Active mode | **1920×1080** |
| Pixel format | **32 bpp** (XRGB8888), stride 7680 |
| `virtual_size` | **1920×3240** → **triple-buffered** (3 × 1080) |
| DRM | `card0` = rockchip display-subsystem; `card1` = **panfrost**; `renderD128` |
| Connector | `HDMI-A-1`: `connected`, `enabled` |

HDMI modes advertised: 1920×1080 (several, incl. interlaced), 1280×1024,
1280×960, 1152×864, **1280×720**, 1024×768, 800×600, 720×576/480.
**No 1280×800** (the native rb UI size).

> Same class of problem PrimeBox hit on the Prime GO: 32 bpp and a
> triple-buffered fb. The rebuilt DirectFB fbdev module must handle that, and
> we must **scale/centre 1280×800** into the HDMI mode (no rotation needed).

## 5. Audio

* Single ALSA card: **`card0: VEYRONHDMI`** with one PCM
  `hw:0,0` (HDMI i2s-hifi), playback + capture.
* Modules: `snd_soc_hdmi_codec`, `dw_hdmi_i2s_audio`, `snd_soc_rockchip_i2s`.
* The `rockchip-snd-max98090` card fails to register (`-517`) — expected, there
  is no analog codec on the Chromebit.
* `aplay -l` (after installing `alsa-utils`) lists `card 0 … device 0`; a
  `speaker-test` on `hw:0,0` runs stereo.

> Audio shim target on the Chromebit = **HDMI stereo (`hw:0,0`)**, not the Prime
> GO's 4-channel JP11 codec.

## 6. USB

* Controller: `usb@ff580000` → **`dwc2` (HS OTG), host mode**, root hub
  `1d6b:0002`.
* Attached: `05e3:0610` USB2.1 hub, `1a2c:7b81` SEMICO keyboard,
  `0781:5581` SanDisk Ultra stick.
* `usb_storage` + `uas` load and bind.
* **The stick took ~17 s to enumerate** (`device descriptor read/64, error
  -110`, then a successful retry). This is exactly why the "retry forever"
  initramfs change matters.
* **No usable gadget port** (`dr_mode = "host"`; the OTG port is the only
  enabled one) → emulate `USB1` at the filesystem layer, not with a USB gadget.

## 7. Input

* `power-button` (`event0`).
* USB keyboard via `hid-generic`/`evdev` (`event1`–`event4`).
* `VEYRON-HDMI HDMI Jack` (`event5`).
* **`keyd` is running**: it grabs the physical keyboard and exposes
  `keyd virtual keyboard` / `keyd virtual pointer` (`event6`/`event7`).
  For keyboard→rb keycode injection, read the keyd virtual device or bypass
  keyd.

## 8. Network / wireless

* Wi-Fi: **Broadcom BCM4354** via `brcmfmac` (SDIO). Connected in the
  reference setup: `<SSID>` → `<device-ip>/24`.
* Bluetooth: BCM4354 (`hci0`/`hci1`).
* `brcmfmac4354-sdio.clm_blob` is missing → "limited channels available"
  (may affect some 5 GHz channels; not blocking).
* `/lib/firmware/mrvl` exists but is unused (the radio is Broadcom).

## 9. Thermals / power

* `cpu-thermal` ≈ 46 °C, `gpu-thermal` ≈ 45 °C at idle.
* No fan; a stick PC with passive cooling. Keep an eye on it under sustained
  `rb` load (software rendering).

## 10. Tooling on the device

| Present | Absent (install as needed) |
|---|---|
| `xz`, `cpio`, `fbset`, `nmcli`/`nmtui`, `busybox` | `gcc`, `make`, `rustc`, `cargo`, `docker`, `7z`, `modetest` |

`alsa-utils` and `evtest` were installed during the survey (this is what
triggered the `mkinitfs` regeneration — now persisted, see
[01](01-postmarketos-image.md#8-making-the-fix-survive-mkinitfs--apk-device-side-persistence)).

## 11. Implications for the `rb` port

| Subsystem | Chromebit reality | Plan |
|---|---|---|
| Float ABI | hard-float host, soft-float `rbp` | soft-float glibc-2.13 chroot (PrimeBox recipe) |
| Display | 32 bpp, triple-buffered, 1920×1080, no 1280×800 | rebuilt DirectFB fbdev + scale/centre |
| Audio | HDMI `hw:0,0` stereo | adapt `audioshim` → HDMI, fold extra channels |
| Input | USB keyboard (+ `keyd`), no touch | rb2go `keyshim`, or USB MIDI (`knobshim2`) |
| USB1 | host-only, no gadget | filesystem emulation (`usb-watch.sh`) |
| Storage | 53 GiB free on `/` after `resize2fs` | plenty for chroot + `gui/` |
| Enumeration | stick appears only after ~17 s | already handled by the initramfs retry loop |
