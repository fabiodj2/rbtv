# 04 — Roadmap / checklist

Ordered, actionable steps. Phases 0 and 1 are done.

## Phase 0 — postmarketOS boots reliably ✅

- [x] Diagnose USB-boot failure (missing `usb-storage` + `hid-generic`)
- [x] Patch initramfs: load USB modules, retry forever, hot-plug rescan
- [x] Rebuild `vmlinuz.kpart` with verbose cmdline
- [x] Enable SSH + Wi-Fi, set hostname
- [x] Produce `images/pmos-veyron-chromebit-fixed.img(.xz)`
- [x] First boot on hardware, SSH reachable

## Phase 1 — Confirm the target environment ✅

- [x] `uname -a` → `Linux chromebit 6.12.96 … armv7l`; DT `google,veyron-mickey-rev8`
- [x] Display: `/dev/fb0` = `rockchipdrmfb`, 1920×1080, **32 bpp**, virtual
      1920×3240 (triple-buffered)
- [x] HDMI modes incl. 1920×1080 … 1280×720 — **no native 1280×800** → will scale
- [x] USB keyboard works (`hid-generic`/`evdev`); `keyd` is also active
- [x] Audio: `card0 VEYRONHDMI`, PCM `hw:0,0` (HDMI) → target for `audioshim`
- [x] Free space: root ext4 resized to 56 GiB (53 GiB free)
- [x] USB enumeration flakiness → automatic port/controller reset added

Full data: [05-chromebit-survey](05-chromebit-survey.md).

## Phase 2 — Build the payload (workstation) ⏳

- [ ] Get XDJ-RX3 v1.20 firmware + key ([PrimeBox](https://github.com/erhan-/PrimeBox) `tools/get-firmware.sh`)
- [ ] Decrypt `.UPD` → ISO (`rx3dec`) and extract (`7z`)
- [ ] Extract `rootfs.cramfs` → soft-float userland
- [ ] Extract `gui.tar.gz` (fonts, imagedata)
- [ ] Patch `rbp` → `rbp-audio`
- [ ] Build shims with `arm-linux-gnueabi-gcc`, verify GLIBC symbol versions
- [ ] Build patched `libdirectfb_fbdev.so` (RGB565→RGB32, fit 1280×800 into HDMI)

## Phase 3 — Ship the chroot ⏳

- [ ] `scp` chroot tarballs to `user@chromebit:/home/user/`
- [ ] Unpack into `/home/user/rbx3-run`
- [ ] Install `rbp-audio`, shims, DirectFB module
- [ ] Create device stubs; block `/dev/mem`
- [ ] Bind `dev/proc/sys/tmp`; `mtab -> /proc/mounts`

## Phase 4 — Bring up subsystems, in order ⏳

- [x] `chroot … /root/pdj/rbp` loads; early `getPcController()` SIGSEGV fixed
      with the `getPcController` guard in `scripts/rb/patch-rbp-crashguards.py`
      (rbp-chromebit, md5 `18a64bc4…`)
- [x] DirectFB opens `/dev/fb0` (fd 10, mmap 24883200 B) and publishes
- [x] UI renders on HDMI, full screen (1280x800 scaled to 1920×1080): the
      driver must publish into physical fb **buffer 0** with `pan=0` (the
      RK3288 VOP ignores the fb pan yoffset) and add the scale/convert path
      (scripts/rb/directfb-chromebit.patch)
- [ ] Keyboard (or MIDI) input reaches `rb`
- [x] Audio opens on HDMI; playhead advances; waveform scrolls —
      [15-playback-fix](15-playback-fix.md) (do **not** apply the
      `getTotalLength` workaround from rb2go's `patch-rbp-debug.py`)
- [x] USB stick detected on the host port; library folder appears; export.pdb
      imported natively (2174 songs / 160 playlists) — [11-usb](11-usb.md)

## Phase 5 — Productise ⏳

- [x] `/home/user/*.sh` launcher scripts (adapt PrimeBox `fix-dev.sh`, `start-rb.sh`)
- [x] systemd units for boot + start/stop: `rb.service` (player + USB watcher),
      `rbkeyd.service`, `ddj400-bridge.service` — see [14-handover](14-handover.md) §4
- [x] Log locations under `/home/user/rbx3-run/tmp`, `/home/user/usbwatch.log`
- [x] Document in this repo ([11-usb](11-usb.md))
- [x] Keep the player in the foreground on HDMI (printk/getty/fbcon, [07](07-display-working.md) §8)

## Phase 6 — Nice-to-haves ⏳

- [x] USB MIDI controller mapping (DDJ-400: transport, mixer, jog, tempo, pads,
      loops, Beat FX) — [12-ddj400](12-ddj400.md)
- [x] Physical keyboard control (rb2go-style, headless evdev) — [13-keyboard](13-keyboard.md)
- [ ] Window mode (rb2go `rbviewer`) if a desktop is added
- [x] Persist the library on a real USB stick / eMMC (native export.pdb import)
- [ ] Auto-select HDMI audio sink
- [ ] Powered hub so stick **and** DDJ-400 can be connected at once ([12](12-ddj400.md) §7)
- [ ] DDJ-400 audio (master out + headphone cue) instead of HDMI ([12](12-ddj400.md) §8)

## Phase 7 — Playback goes silent ⏳ (next session)

- [ ] **Loaded track + PLAY does not play / is silent** — full evidence and the
      ordered next steps are in [14-handover](14-handover.md) §6; the audio
      device itself is healthy (PCM RUNNING, ALSA callback counting, but
      `peak_m=0`), so first prove the transport state (fb0 hash probe) and the
      button path (`press-key.sh play 1`), then the mixer values the engine
      really holds.

## Open questions

* Does the Chromebit HDMI driver expose a 1280×800 mode, or do we scale?
* Which HDMI PCM format (S16_LE vs S24_LE) does rockchip-hdmi-audio use here?
* Is a given USB MIDI controller's protocol close enough to reuse `knobshim2`
  directly, or does it need a mapping table?
* Root is on the internal eMMC now — the USB stick is purely the music library.
* Why is the mixer output silent although the callback runs — do the synthetic
  mixer defaults actually land in the engine, and do the faders/crossfader reach
  the right channels?