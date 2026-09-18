# 17 — Testing: static checks + device smoke tests

Two layers, because most of this repo is shell/C glue that targets hardware
we can't run in CI, and a couple of daemons that need a live device.

## 1. Static checks (CI, no hardware)

`.github/workflows/ci.yml` runs on every push/PR:

| Job | What | Scope |
|---|---|---|
| `shellcheck` | lints every `*.sh` at `-S error` (blocking); `-S warning` runs too, informationally | all shell scripts except `scripts/fix-image/patched/` (vendored postmarketOS initramfs, GPL, not ours to restyle) |
| `c-syntax` | `arm-linux-gnueabi-gcc -fsyntax-only -march=armv5t -mfloat-abi=soft` against the real target ABI used by [`shims-Makefile`](../scripts/rb/shims-Makefile) | `scripts/rb/*.c`, `scripts/rb/device/*.c` |
| `python-syntax` | `python3 -m py_compile` | `scripts/rb/patch-rbp-crashguards.py` |

Run the same checks locally:

```sh
sudo apt-get install -y shellcheck gcc-arm-linux-gnueabi
find . -name '*.sh' -not -path './.git/*' -not -path './scripts/fix-image/patched/*' \
  | xargs shellcheck -S error
for f in scripts/rb/*.c scripts/rb/device/*.c; do
  arm-linux-gnueabi-gcc -fsyntax-only -march=armv5t -mfloat-abi=soft \
    -Wall -Wextra -Wno-unused-parameter -std=gnu99 "$f"
done
```

**Limits.** `-fsyntax-only` catches type/syntax errors with the correct
target ABI, but it is not a full build: the shims are `LD_PRELOAD`ed into the
proprietary XDJ-RX3 rootfs (`RX3=...` in `build-shims.sh`), which is firmware
we neither ship nor fetch here (see [NOTICE.md](../NOTICE.md)). A real build
still needs that rootfs as the link sysroot — see
[06-rb-build](06-rb-build.md). `scripts/rb/probe/dfbtest.c` needs DirectFB
headers from that same rootfs, so CI skips it.

## 2. Device smoke tests (needs the Chromebit)

[`scripts/rb/device/selftest.sh`](../scripts/rb/device/selftest.sh) gives a
single PASS/FAIL verdict instead of the raw dumps from
[`check-display.sh`](../scripts/rb/device/check-display.sh),
[`check-audio.sh`](../scripts/rb/device/check-audio.sh) and
[`usb-probe.sh`](../scripts/rb/device/usb-probe.sh):

| Check | Required? |
|---|---|
| `rb.service` active | required |
| `rbp` process running | required |
| HDMI connector `connected` + `/dev/fb0` present | required |
| ALSA `card0`/`pcm0p` (HDMI PCM) present | required |
| chroot artefacts present: `rbp`, DirectFB fbdev driver, `memshim`/`audioshim`/`keyshim` | required (existence); md5 compared too if `EXPECT_*_MD5` is set |
| `rbkeyd.service`, `ddj400-bridge.service` | optional — depend on what's plugged in |
| USB1 library detected (`usb-probe.sh -q`) | optional — depends on a stick being plugged in |

Run it directly on the device:

```sh
sh /home/user/selftest.sh          # human-readable
sh /home/user/selftest.sh -q       # one summary line, for scripting
```

Or from the workstation, right after a deploy
([`deploy-module.sh`](../scripts/rb/deploy-module.sh) /
[`deploy-chromebit.sh`](../scripts/rb/deploy-chromebit.sh)), with
[`run-device-tests.sh`](../scripts/rb/run-device-tests.sh):

```sh
PASS=<password> HOST=root@chromebit.local ./scripts/rb/run-device-tests.sh
```

**Comparing deployments.** To check a freshly deployed artefact against a
previously known-good one (e.g. the hashes recorded in
[16-handoff §1](16-handoff.md)), export the matching `EXPECT_*_MD5`:

```sh
PASS=<password> \
  EXPECT_RBP_MD5=18a64bc4d0ffd1cbd35f3a6ea447fca8 \
  EXPECT_FBDEV_MD5=ef8e336a4dac761bc23191c6446d6bc3 \
  EXPECT_MEMSHIM_MD5=867a19abc6174432a835bfc5e0e924f2 \
  EXPECT_AUDIOSHIM_MD5=73b3d6d4e270133a5a6733a68b66fbbd \
  EXPECT_KEYSHIM_MD5=8875de779d6c048c0433e6730b25aec5 \
  ./scripts/rb/run-device-tests.sh
```

A mismatch fails that check, which is the quickest way to notice "the device
is running an older/different build than I think" — the recurring class of
bug in [16-handoff §2](16-handoff.md).

Exit code is 0 only if every required check passes; use it as a gate after a
deploy before trusting the device for a gig or a longer test session.
