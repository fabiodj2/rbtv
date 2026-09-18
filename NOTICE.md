# Notice, copyright and legal

This is an independent **interoperability / preservation** project. It is not
affiliated with, endorsed by, or sponsored by Pioneer DJ, AlphaTheta, ASUS,
Google, Rockchip, Denon DJ / inMusic, the postmarketOS project, Xiaomi, or any
of their subsidiaries.

## What this repository contains

* Original shell scripts, documentation and image-build notes written for this
  project — **MIT** licensed (see [LICENSE](LICENSE)).
* Initramfs integration files for the Chromebit USB-boot fix — modified
  copies of, and patches against, the postmarketOS initramfs. That upstream
  code is **GPL-2.0-or-later**; the copies in
  [`scripts/fix-image/patched/`](scripts/fix-image/patched/) and the patches
  derived from them keep that licence (see
  [`scripts/fix-image/patched/README.md`](scripts/fix-image/patched/README.md)
  and the `COPYING` there). Everything else in this repository is MIT.
* Image-build notes and fix assets: [`scripts/fix-image/`](scripts/fix-image/)
  documents how the USB-boot fixes are applied to the upstream postmarketOS
  image. The flashable image itself is **not** bundled (large binary); get the
  pristine upstream image from <https://postmarketos.org/>.
* No proprietary firmware or vendor binaries beyond what postmarketOS/Alpine
  already ship in their own image.

## What this repository does **not** contain

* No XDJ-RX3 firmware (`.UPD`), no decrypted firmware ISO, no `rootfs` and no
  `rbp`/`rb` player binary — these are Pioneer/AlphaTheta property. You supply
  your own legally obtained firmware. The tooling to extract and patch it lives
  in the related [PrimeBox](https://github.com/erhan-/PrimeBox) and `rb2go`
  projects.
* No **firmware decryption key** (`aes256.key`). AlphaTheta published the key
  in their own GPL source distribution; you obtain it yourself. It is
  gitignored here.
* No rekordbox music, playlists, analysis data or databases (`export.pdb`), and
  no Denon / Engine OS files.

## Legal caveats

* Firmware decryption and reverse-engineering may be restricted in your
  jurisdiction (e.g. DMCA §1201, EUCD). Check your local law.
* Patching and running a vendor application on third-party hardware may violate
  the vendor's EULA. This project is offered for research, repair, preservation
  and personal interoperability only.
* Installing anything on your Chromebit is at your own risk. It can brick the
  device or void warranties.

## Trademarks

*Pioneer DJ*, *AlphaTheta*, *rekordbox*, *XDJ-RX3*, *CDJ* are trademarks of
their respective owners. *Denon DJ* and *Prime GO* are trademarks of inMusic.
*ASUS* and *Chromebit* are trademarks of ASUS. *Google*, *Chrome OS* are
trademarks of Google. *Rockchip* and *RK3288* are trademarks of Rockchip.
All trademarks are used here in a descriptive, nominative sense only.

## Credits

* postmarketOS — the OS this runs on.
* [nsaintot/cdj3k-emu](https://github.com/nsaintot/cdj3k-emu) — Pioneer `.UPD`
  decryption groundwork.
* The [PrimeBox](https://github.com/erhan-/PrimeBox) and `rb2go` projects — the working `rbp` port.
* DirectFB, JUCE, ALSA and the other open-source components inside the RX3
  firmware.
