# Consolidation plan: Orange Pi RK3399 + DDJ-400 (2026-10-02)

## Goal
Consolidate what runs on the Orange Pi 4 LTS into the RX3-Orange-PI-4-LTS repo, with a central config, a NOTICE with origins and no firmware. Chosen by the user over "make rx3-build portable" and "review only".

## Facts (from the board and GitHub)
- Working tree on the board: `/home/orangepi/RX3-RK3399-DDJ400`, branch `port/rk3399-ddj400` (also `display/1024x768-scale` at HEAD). Remotes: `github` and `upstream` = fabiodj2/rbtv (upstream push disabled), `toolkit` = Tratosca/rx3-toolkit (MPL-2.0 source of the runtime patches).
- It is **9 commits ahead** of `rbtv:port/rk3399-ddj400` (head on GitHub `a23caca`, 2026-09-24). Those commits exist only on the board. Latest local commit `27f6fb3` (BEAT SYNC LED). Others: dedupe FILTER CC, SHIFT+platter SEARCH keys, SHIFT+BEAT FX TAP, Sound Color FX select, pad-bank sub-layer, mixer sync at startup.
- Uncommitted on the board: modified `docs/12-ddj400.md`, `docs/README.md`, `scripts/rb/audioshim.c`, `scripts/rb/device/ddj400-bridge.c`, `scripts/rb/start-rk3399-ddj400.sh`; new `docs/20-flicker-zoom-fix.md`, `scripts/rb/.start-waveform-drm-test-20260926.sh`; stray `bootstrap-claude.sh` (must not be committed).
- The `rx3-build` backup (`RX3-Orange-PI-4-LTS` branch `backup/orangepi-2026-10-02-orangepi4-lts-snapshot-v2`) is the rx3-pi (Raspberry Pi 5) derivative with hardcoded `/home/pompu_5`, 1200x1920 and FLX6 assumptions, not validated on the board. Reference only.
- `rbtv:port/rk3399-ddj400` carries 94 `.rgb565` overlay images (key-stems) and runtime patches adapted from Tratosca/rx3-toolkit. Provenance of the images is **not yet established** (generated vs extracted from firmware).

## Blockers before anything is pushed
1. Both repos are still public as of the last check. The user's `gh repo edit` failed because `gh` is not authenticated on the Mac. `rbtv` is a fork and may not be switchable to private.
2. The 9 commits and the uncommitted work are unprotected. Interim safeguard that publishes nothing: `git bundle` of `github/port/rk3399-ddj400..HEAD`, `git diff` patch and a tarball of the new files, copied to the Mac with `scp`.
3. Provenance of the `.rgb565` assets and the rbp patch bytes.

## Next steps
1. User: bundle + patch to the Mac; paste `git log --oneline github/port/rk3399-ddj400..HEAD` and `git diff --stat`.
2. Review those names for firmware-derived or secret content.
3. Once the target repo is private: create a consolidation branch in RX3-Orange-PI-4-LTS from `port/rk3399-ddj400` plus the bundle, without `work/` or firmware.
4. Add a central config (paths, UIDs, geometry, devices) and a NOTICE with origins; run security-reviewer and `verify.sh`; then push.

## Progress (later on 2026-10-02)
- Repository RX3-Orange-PI-4-LTS is private (anonymous check returned 404). Imported branch `import/rk3399-ddj400-board` (9 commits + the uncommitted work, commit `6e520a9`).
- Consolidation branch `consolidate/rk3399-ddj400` (commit `c9fb4ed`): central `config/rx3.conf` with the previous values as defaults, a validating loader `scripts/rb/rx3-config-lib.sh`, the launcher reading from it, `docs/21-central-config.md`, NOTICE with upstream origins. Not yet run on the board.
- Two independent security reviews of the launcher change (it runs as root): fixed regex injection into `pkill`, config sourced from the environment as root, `stop` failing on a broken local file, unvalidated numeric values, and tests that missed bugs. Final suite: 47 checks as root (37 without), 16 of 16 reintroduced bugs caught.
- docs/20 corrected: the RGB16 flicker fix is NOT applied (code and running processes use ARGB).

## Still open
1. Provenance of the 94 `.rgb565` images and the patch byte candidates (generated vs derived from firmware): the user has not answered. Keep the repo private until settled.
2. Trying the new launcher on the board: run `sh scripts/rb/start-rk3399-ddj400.sh config` first and compare with the live process arguments, run the test as root, then restart the service in a quiet moment.
3. Remaining hardcoded paths: systemd units (`/home/orangepi/RX3-RK3399-DDJ400`), `usb-watch.sh`, `assemble-rk3399.sh`, `manage-functional-baseline.py`, `deploy-module.sh`, key-stems `module.sh`.
4. Real-time scheduling for the service as a normal user (rbtv docs/18).
5. `rbtv` (public fork) still holds `port/rk3399-ddj400`; decide whether to make it private or duplicate it.

