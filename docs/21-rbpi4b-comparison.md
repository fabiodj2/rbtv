# 21 — rbpi4b vs. this port (Orange Pi 4 LTS / RK3399)

[fabiodj2/rbpi4b](https://github.com/fabiodj2/rbpi4b) (fork of
salesforcedoc/rbpi4b, 54 commits, read at `e3de2c1` on 2026-10-10) runs the same
XDJ-RX3 `rbp` v1.20 on a Raspberry Pi 4B with an HDMI touch monitor and a
DDJ-FLX4. Same architecture as ours: soft-float glibc 2.13 chroot,
`LD_PRELOAD` shims, DirectFB on fbdev. This file records what was compared,
what was taken, and what was left, with the evidence.

## Side by side

| | rbpi4b (Pi 4B) | this port (Orange Pi 4 LTS) |
|---|---|---|
| Display | DirectFB present path in the driver (off/convert/letterbox/scale), `video=HDMI-A-1:1280x800` on the kernel cmdline | `fbshim32` virtual 1280×800 fb + `fb-present` pillarbox to 1920×1080×32 (1 page) |
| Touch | in-process `tscfake.c` pipe + `pointsrc.c` discovery, mouse fallback with a drawn cursor, many shim-drawn touch zones | host `rx3-touch-bridge` → FIFO `/dev/tsc2007_2-0048` → `memshim` read |
| Controller | FLX4, tables from Pioneer's list, not all verified | DDJ-400 mapped and verified live (docs/12) |
| Service | `Type=simple`, `Restart=always`, `KillMode=mixed`, `Conflicts=getty@tty1`, starts at basic.target | was `oneshot`+`RemainAfterExit`, network-online + `sleep 8`, disabled |
| Diagnostics | `doctor.sh` (read-only), `healthwatch`, persistent journal | none until this change |
| Tests | qemu-arm unit tests for the shims | none |

## Finding: why our touchscreen never worked

Measured on the unit, 2026-10-10, against the player that had been running since
2026-10-01:

* `rx3-touch-bridge` saw every touch (`touch begin ... region=0` in touch.log)
  and wrote it. The FIFO held **19 416 bytes unread** (3 236 records). `rbp` had
  never read one.
* The `TouchPanel` thread was always in `nanosleep`. Its stack went
  `TouchPanel::run` → `commRxDataProc` → `TouchPanelComm::readFd` → `read@plt` →
  **`memshim.so`+0xa04** → nanosleep. That is memshim's no-bridge fallback,
  which returns a zeroed record every 20 ms.
* The deployed `memshim.so` (md5 `79062131`) was the 2026-09-18 21:27 build. It
  has no `RX3_TOUCH_BRIDGE` string at all. The read path that honours it was
  added to `memshim.c` on 2026-09-19 and never rebuilt, because
  `build-shims.sh` needs a PrimeBox checkout that this machine does not have.

This rules out the hypotheses left open on 2026-10-01: the touch-panel mode at
`+0x3c` and the disconnected DP-1 panel. The fbshim already answers the tsc2007
ioctls (`0x6b` magic). The `ENOTTY` in the 2026-09-30 strace came from a run
without fbshim in `LD_PRELOAD`.

## Taken from rbpi4b

1. **Touch record contract** (rbpi4b docs/07, "rbp reflects x", "Required
   files"). The record carries logical 1280×800 coordinates with
   `x = 1279 - lx`, and `root/settings/TouchCalib_{User,Factory}.dat` hold the
   identity `0 0 320 200 1280 800`. Our bridge used to send raw-ADC style values
   (`37+(1280-x)*3976/1280`, `72+y*3856/800`), which assume a factory
   calibration that this runtime never had: both files were missing (ENOENT in
   the strace). The launcher now installs the identity files, and keeps a
   `.bak` of any different file. **Verified by finger 2026-10-10**: sidebar icon,
   track list and INFO all act where touched.
2. **First-touch seed** (rbpi4b `seed_abs_position`). The kernel drops an ABS
   event equal to the stored value, so the bridge now seeds every slot from
   `EVIOCGABS` at connect.
3. **Doctor** (`scripts/rb/device/doctor-rk3399.sh`, from rbpi4b `doctor.sh`).
   It is read-only. It checks each shim mapped in `/proc/<rbp>/maps` against a
   build artifact, memshim's bridge path, the calibration files, the touch FIFO
   backlog, the rbp patch words and the fb/connector state. It would have caught
   this bug on day one.
4. **Shim build without PrimeBox** (`scripts/rb/build-rk3399-shims.sh`). It
   uses the runtime chroot as sysroot and refuses a `.so` needing glibc > 2.13.
5. **Service unit** in the rbpi4b style, plus a blocking `run` mode in
   `start-rk3399-ddj400.sh`, plus `journald-persistent.conf`. These are shipped
   in the repo, **not installed** by this change.

## Checked, already fine here

* `scanForDevices` @ `0x3c665c` is already `0xea000054` in the deployed rbp, so
  this is not our ALSA crash.
* fb0 is `rockchipdrmfb` 1920×1080×32 with a single page. rbpi4b's
  "3 buffers on a 1-page fb" fix only matters if tearing shows up. Our fbshim
  already presents a fake 1280×800.

## Not taken (yet), and why

| rbpi4b item | Status here |
|---|---|
| `patch-rbp-nopc.py` (getPcController NULL only when UiObjManager is NULL) | Our rbp has the `mov r0,#0; bx lr` stub at `0x31df64`, which disables Pro DJ Link / USB-B PC link. Only matters if we want those. It is a binary patch to the live player, so it needs a backup and a separate decision. The doctor WARNs. |
| DirectFB present path diff | Not needed: our fbshim32 + fb-present path works. It would be a third DirectFB lineage. |
| Shim-drawn touch zones (top menu, edge drawers, FX pad, HOT CUE row, waveform swipe) | Big and FLX4/Pi-oriented. Candidates one at a time, now that base touch works. |
| `RB_AUDIO_FMT` / channel map in audioshim | DDJ-400 audio already works. Possibly relevant to the open master-click issue: check `/proc/asound/card*/stream0` format first. |
| `boot-trim.sh`, `display-watch.sh`, `healthwatch` | `boot-trim` edits Pi `/boot/firmware` and does not apply to Armbian. The other two are worth porting later. |
| qemu-arm shim tests | Worth it eventually. Not started. |
