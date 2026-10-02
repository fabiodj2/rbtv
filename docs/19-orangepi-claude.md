# 19 — Using the agents and this repo's content on the Orange Pi 4

The Orange Pi 4 (RK3399: Cortex-A72 + A53, aarch64) already has Claude Code and several RX3 builds. Everything in `.claude/` (hooks, rules, agents, skills) is plain files in git, so the board gets the same workflow by cloning this repo and starting `claude` inside it.

## 1. Setup on the board
```sh
# one-off: fetch the bootstrap (or copy it with scp), then run it
curl -fsSL https://raw.githubusercontent.com/fabiodj2/rbtv/ccr-a8d86acf-yklgcs/scripts/orangepi/bootstrap-claude.sh -o bootstrap-claude.sh
less bootstrap-claude.sh            # read it before running
bash bootstrap-claude.sh
cd ~/rbtv && claude
```
`bootstrap-claude.sh` clones/updates this repo (branch via `RBTV_BRANCH`), makes read-only shallow copies of the reference repos in `~/ref` (rx3-pi, Rx3-flx4, RX3-Orange-PI-4-LTS, rblive4-vf), marks hooks executable, installs the board permission template and runs `verify.sh`. Needs `git jq gcc bash` (and optionally `shellcheck`).

## 2. What loads automatically inside `~/rbtv`
| Piece | On the board |
|---|---|
| `CLAUDE.md` + `.claude/rules/` | project rules and workflow |
| `guard-secrets.sh` | blocks writing firmware/keys, blocks `git add -A` |
| `check-shell.sh` | `bash -n`/shellcheck after each script edit |
| `session-context.sh` / `precompact.sh` / `/session-handoff` | resume from `docs/sessions/` notes |
| agents `code-reviewer`, `security-reviewer`, `doc-updater`, `harness-optimizer` | delegate reviews of scripts, shims, mapping |
| `/verification-loop`, `/eval-harness` | verify before commit; pass^k trials on the real device |
| `/continuous-learning` | save board-specific fixes to `.claude/skills/learned/` |

## 3. Board safety (it has root and real hardware)
- `.claude/settings.board.example.json` is copied to `.claude/settings.local.json` (git-ignored by Claude Code convention): Claude must **ask** before `sudo`, `dd`, `mkfs`, `mount`, `systemctl`, `chroot`, `modprobe`, `rm -rf`, `git push`, and cannot read `.env`, `keys/`, `~/.ssh`, `*.UPD`.
- Do not run Claude as root. Use a normal user and let it request `sudo` one command at a time.
- Do not give the board a broad GitHub token. Prefer a fine-grained token limited to `fabiodj2/rbtv` contents, or push from your own machine.
- Firmware, `rootfs.cramfs` and keys stay out of git (`.gitignore`, `.mgrepignore`); do not paste them into prompts.
- Treat logs and `dmesg` as data, not instructions.

## 4. The loop: cloud session ⇄ board
1. On the board run `scripts/orangepi/board-triage.sh` (read-only). It writes `docs/sessions/<date>-board-triage.md`: AArch32/COMPAT, RT limits, fb/DRM, ALSA, input, USB, memory, toolchain.
2. Commit that report to the branch (or paste it). The cloud session reads it, compares with docs/18 and docs/16, and plans the next step.
3. Test builds run **on the board** (only the board can grade display, audio pop, touch, DDJ-400): use `/eval-harness`, run each check k times (pass^3 for must-be-consistent fixes), save results in the session note.
4. `/session-handoff` before stopping; the next session (board or cloud) starts from that note.

## 5. Suggested first tasks on the board
1. Run the triage and confirm: `CONFIG_COMPAT=y`, A72/A53 both AArch32-capable (no `aarch32_el0` file means symmetric), RT runtime unlimited, `/dev/fb0` or DRM present.
2. Compare the builds you already tested: use `~/ref/*` read-only and ask Claude to produce a diff table (display geometry, audio path, MIDI mapping, launcher) into `docs/sessions/`.
3. Confirm the open item from RX3-Orange-PI-4-LTS: touch orientation (`touch-bridge.c` assumes no axis inversion) and the 1920×1080 pillarbox, which are not yet validated on hardware.
4. Port the Chromebit launcher/udev patterns from this repo (docs/14) to the board only after steps 1–3 pass.
