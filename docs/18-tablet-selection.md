# 18 — Tablet selection for a native XDJ-RX3 `rbp`

Status: **research, partly unverified.** Sources: this repo, the user's other repos (rx3-pi, Rx3-flx4, RX3-Orange-PI-4-LTS, rblive4-vf, az-*) and web searches. Anything marked *unverified* comes from search-result summaries only; the postmarketOS wiki and GitLab were blocked by the environment's network policy, so no per-device support status could be confirmed.

## 1. Hard requirements
1. **CPU executes ARM32 (EABI5 soft-float) natively**: armv7, or aarch64 with working AArch32 EL0 (`CONFIG_COMPAT`). The host float ABI does not matter; the chroot carries glibc 2.13 (docs/02).
2. **Linux we control**: root, chroot, bind mounts and real-time scheduling (`kernel.sched_rt_runtime_us=-1`, `ulimit -r 99`). Without RT, `acre_tsk` fails and `rbp` segfaults (Rx3-flx4 `PI-SETUP-NOTES.md`). Locked-bootloader Android tablets are out.
3. **Display**: `/dev/fb0` (needs `CONFIG_DRM_FBDEV_EMULATION` + `CONFIG_FB_DEVICE` on DRM-only kernels; *unverified for DirectFB 1.4*). UI is 1280×800 RGB565, scaled in software. Display must be present at boot (fb is not created on hotplug).
4. **Audio**: a real ALSA card (HDMI PCM, USB audio class, or the DDJ-400/FLX4 as sound card). RX3 expects 3 DACs, so a shim maps them (docs/08).
5. **USB host with VBUS power** for the library stick and a MIDI controller. Most tablets have one OTG port: plan on a powered hub (the DDJ-400 cannot be fed by a bus-powered hub, docs/12).
6. **Touch via evdev**: the shim emulates `tsc2007` from any evdev device; calibration and rotation are needed.
7. RAM: 1 GiB is tight, 2 GiB is proven (RK3288). Chroot is about 55–60 MB.

## 2. SoC triage
| Class | Verdict | Evidence |
|---|---|---|
| RK3288 (Cortex-A17, armv7) | **Proven** | Chromebit, Prime GO, SC Live 4 on kernel 6.x |
| Raspberry Pi 5 (Cortex-A76, aarch64 + compat) | **Proven** | rx3-pi, Rx3-flx4 (daily driver, Touch Display 2, FLX4/DDJ-400) |
| RK3399 (Orange Pi 4) | Untested | RX3-Orange-PI-4-LTS only compiled |
| Cortex-A53/A55/A72/A73 (RK3566/68, A64/H6/H616) | Plausible, *unverified* | Arm docs say AArch32 is possible; needs kernel compat; one secondary source for RK3568 |
| RK3588 (A76+A55) | *Unverified* | AArch32 on A76 per Arm; kernel/board support not confirmed |
| Armv9.2 cores (Cortex-X4/A720/A520: Snapdragon 8 Gen 3, similar) | **Avoid** | 64-bit only (Arm A720 docs, 9to5google, Android Police) |
| Asymmetric AArch32 SoCs | Avoid | need `allow_mismatched_32bit_el0`, `/sys/devices/system/cpu/aarch32_el0` (docs.kernel.org) |

Lowest risk is native armv7: it avoids the 32-bit-userspace-on-64-bit-kernel ioctl compat layer (ALSA `snd_pcm_status32`, fbdev, evdev). Compat behaviour for this glibc 2.13 binary is *unverified*.

## 3. Candidate paths
1. **Pi 5 + Touch Display 2 in a tablet enclosure.** Proven end to end by the user's repos. Recommended.
2. **RK3288 10.1" 1280×800 tablet.** Known SoC, native panel size. No model with mainline/postmarketOS support is verified (only veyron Chromebooks/Chromebit are known); a model must be identified first.
3. **Wiki-listed tablets, hypotheses only (*unverified*):** Nexus 7 2012 (Tegra 3, 1280×800, 7"), Galaxy Tab 2 10.1 (OMAP4, 1280×800), Microsoft Surface RT (Tegra 3, needs bootloader exploit), PineTab (A64, aarch64: AArch32 untested). Do not buy before checking page status, touch, audio and USB host.
4. **Avoid:** stock Android tablets with locked bootloaders; any 2024+ flagship SoC.

## 4. Triage checklist (run on any candidate before buying/flashing)
```sh
grep -i -E 'aarch32|Features' /proc/cpuinfo; lscpu
zcat /proc/config.gz | grep -E 'COMPAT|AARCH32|DRM_FBDEV|FB_DEVICE'
cat /sys/devices/system/cpu/aarch32_el0     # exists => asymmetric 32-bit
./hello32                                    # static ARM32 test binary
ls /dev/fb* /dev/input /dev/snd; aplay -l; cat /proc/asound/cards
lsusb -t; dmesg | grep -i -E 'usb|otg'      # check VBUS and port count
```

## 5. Known pitfalls from the user's repos
- Link shims against the RX3 glibc, not the host (host gives GLIBC_2.17/2.34 symbols and breaks).
- Without `directfbrc` `no-hardware` the kernel can oops; the GPU is never used.
- Start-up audio pop (~200 ms) is open (docs/16 §4): needs mute/ramp in the shim.
- Controller hotplug can loop xrun: handle lost PCM in the shim; FLX4 needs SysEx keep-alive.
- `getPcController()` NULL and `getTotalLength()` crashes need `rbp` patches.
- USB enumeration can take ~17 s (also on the Chromebit); the Pi 5 needs per-port VBUS control.

## 6. Open items
- Re-run the candidate research once `wiki.postmarketos.org` and `gitlab.postmarketos.org` are allowed by the environment's network policy, then fill a per-device table (SoC, panel, touch, audio, USB, bootloader, source URL).
- Decide between path 1 (Pi 5) and path 2 (RK3288 tablet) with the user.
