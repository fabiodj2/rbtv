# RX3 Toolkit integration: staging and hardware validation

## Current state (22 September 2026)

The `backup/functional-2026-09-22` branch at `4ce92254df52c8a3a5ac62801555503be68e9782` is the protected baseline. Work is on `feature/rx3-toolkit-runtime`. This repository's `start-rb.sh` describes a Chromebit chroot. No verified Orange Pi 4 LTS `rbp` binary, installed runtime, audio stem engine, or hardware test exists in this checkout. Do not equate an offline staged file with a working RX3 feature.

The modular loader and offline patch manager are isolated from the launcher. The patch manager refuses any manifest with an empty or incorrect SHA-1, any byte mismatch, overlap, out-of-bounds patch, or existing output. The `beatjump-rx3-1.19.candidate.json` is *disabled* with an empty hash. It contains upstream candidate offsets, not validated offsets for our binary. Existing PrimeBox/Chromebit patches may change the hash or instructions at these sites. No KEY/STEMS renderer or audio engine has been integrated.

## 1. Obtain code and verify the baseline

```sh
git clone https://github.com/fabiodj2/rbtv.git
cd rbtv
git fetch origin
git switch feature/rx3-toolkit-runtime
git rev-parse backup/functional-2026-09-22
git log --oneline backup/functional-2026-09-22..HEAD
sh scripts/rb/runtime/tests/test-runtime.sh
python3 -m unittest discover -s scripts/rb/runtime/tests -p 'test_*.py' -v
```

Expect the baseline hash shown above and six patch manager tests passing. `git diff --name-status backup/functional-2026-09-22...HEAD` shows the newly added integration files; existing launcher and shims should remain unchanged.

## 2. Inspect the exact target binary (on your development machine)

Copy your *legally obtained* unmodified target `rbp` to a private directory outside the Git repository. Do not upload the firmware or `rbp` to GitHub. Substitute the actual path in the commands below.

```sh
RBP_STOCK="$HOME/rx3-private/rbp.stock"
file "$RBP_STOCK"
sha1sum "$RBP_STOCK"
sha256sum "$RBP_STOCK"
cp scripts/rb/runtime/patches/beatjump-rx3-1.19.candidate.json "$HOME/rx3-private/beatjump-reviewed.json"
python3 - "$RBP_STOCK" "$HOME/rx3-private/beatjump-reviewed.json" <<'PY'
import json, pathlib, sys
binary = pathlib.Path(sys.argv[1]).read_bytes()
spec = json.loads(pathlib.Path(sys.argv[2]).read_text())
for patch in spec['patches']:
    o = patch['offset']; before = bytes.fromhex(patch['before'])
    actual = binary[o:o+len(before)]
    print(f"{o:8d} {patch['label']}: expected={before.hex()} actual={actual.hex()} match={actual == before}")
PY
```

If any entry says `match=False`, **stop**: identify the matching instruction and surrounding function for that exact firmware and architecture before preparing a new manifest. Matching bytes alone is necessary but does not prove equivalent code behavior. Compare disassembly and verify the binary's ARM mode, firmware lineage, load layout, display assets, and current crashguard patches. The upstream 1.19 hash list is not a substitute for the hash of the exact stock input being patched.

## 3. Stage only after independent binary analysis

After reviewing every candidate offset and its semantics, set `source_sha1` in the private manifest to the hash of the **actual input**. If offsets differ, change them with independently established addresses and expected bytes. Do not commit the private binary or the reviewed manifest containing proprietary excerpts.

```sh
python3 - "$RBP_STOCK" "$HOME/rx3-private/beatjump-reviewed.json" <<'PY'
import hashlib, json, pathlib, sys
source = pathlib.Path(sys.argv[1])
manifest = pathlib.Path(sys.argv[2])
spec = json.loads(manifest.read_text())
spec['source_sha1'] = hashlib.sha1(source.read_bytes()).hexdigest()
manifest.write_text(json.dumps(spec, indent=2) + '\n')
PY
python3 scripts/rb/runtime/patch-manager.py verify --source "$RBP_STOCK" --manifest "$HOME/rx3-private/beatjump-reviewed.json"
python3 scripts/rb/runtime/patch-manager.py stage --source "$RBP_STOCK" --manifest "$HOME/rx3-private/beatjump-reviewed.json" --output "$HOME/rx3-private/rbp.beatjump-staged"
sha1sum "$RBP_STOCK" "$HOME/rx3-private/rbp.beatjump-staged"
```

`verify` does not retain a patched binary. `stage` writes a new one. The CLI will refuse to overwrite an existing output. Do not deploy this stage to a live player without a specific hardware acceptance and recovery procedure.

## 4. Hardware acceptance and rollback

First verify the current player, audio MASTER/CUE, DDJ-400 MIDI mappings, display and playback with the stock binary. Save a second copy of the known working runtime outside the deployment path and its SHA-256. Schedule a test with local console access and a bootable backup. Only then test the staged binary in an isolated deployment with manual operator supervision; test boot, full track playback, repeated Beat Jump backward/forward, quantize on/off, waveform, LEDs and a restart. If startup, controls or playback regress, stop the test and revert the deployment to the saved known-working runtime. The offline helper can create a restored **copy** from the preserved stock source:

```sh
python3 scripts/rb/runtime/patch-manager.py restore --source "$RBP_STOCK" --output "$HOME/rx3-private/rbp.restored"
sha256sum "$RBP_STOCK" "$HOME/rx3-private/rbp.restored"
```

There is no automatic device rollback or `systemd` installation in this branch. `restore` does not replace an installed file. Deployment paths differ between the Chromebit repository and the Orange Pi project, so do not guess a `cp` or `systemctl` command.

## Remaining integration gates

1. Obtain the exact Orange Pi runtime repository/build and exact `rbp` plus existing patches; document whether this branch is the correct target.
2. Validate all Beat Jump offsets by disassembly and test on real hardware.
3. Design an opt-in launcher with atomic install and watchdog rollback for the target system; retain the stock service path until verified.
4. Port and cross-compile performance core against the actual ARM32 ABI and graphics hooks; test display and touch.
5. Implement KEY control/audio algorithm and per-deck state; verify DDJ-400 mapping against existing MIDI bindings.
6. Implement offline stem preparation, sidecar association, synchronized dual-deck audio mixing, CUE/MASTER routing and UI; measure CPU and latency on RK3399.

The upstream MPL-2.0 candidate bytes have attribution in `scripts/rb/runtime/patches/NOTICE.md`; review upstream license obligations when incorporating its code.
