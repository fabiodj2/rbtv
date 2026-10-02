# 20 — Standard path for Orange Pi backups

Decision: every backup of the Orange Pi (tested RX3 builds, shims, launchers, configs) goes to git through the **MacBook**, with `scripts/orangepi/backup-pull.sh`. The board never holds GitHub credentials and never pushes.

Target repo: **fabiodj2/RX3-Orange-PI-4-LTS** (the Orange Pi port). Each backup is its own branch `backup/orangepi-<YYYY-MM-DD>-<label>` created from the repo's default branch; the default branch is never touched by the script.

## One-time setup on the MacBook
```sh
git clone https://github.com/fabiodj2/RX3-Orange-PI-4-LTS ~/rx3-orangepi     # backup target
git clone -b ccr-a8d86acf-yklgcs https://github.com/fabiodj2/rbtv ~/rbtv    # holds the script
# ~/.ssh/config
Host orangepi
  HostName fe80::d023:fbff:fe18:8f4f%en6     # link-local; update if it changes
  User orangepi
```
Needs `git`, `rsync`, `ssh` (all present on macOS; works with bash 3.2).

## Each backup
```sh
BACKUP_REPO=~/rx3-orangepi ~/rbtv/scripts/orangepi/backup-pull.sh [--no-push] [--allow 'glob'] <label> '<remote-path>' ['<remote-path>' ...]
# example
BACKUP_REPO=~/rx3-orangepi ~/rbtv/scripts/orangepi/backup-pull.sh rx3-v3-working '~/rx3-handoff' '~/start-rx3.sh'
```
Options: `--no-push` keeps the branch local; `--yes` skips the push question; `--allow '<name-glob>'` (before the label) stores matching binaries/archives such as your own `keyshim*.so`.
Env: `BACKUP_REPO` is **required**; `BACKUP_EXPECT_REPO` (default `rx3-orange-pi-4-lts`) must appear in the target's origin URL or the script refuses; `OPI_HOST` (default `orangepi`); `OPI_BACKUP_MAX_SIZE` (default 50m).

Steps the script performs:
1. **Validate** label, host and remote paths. Paths may only contain `A-Za-z0-9._/~+-` (no spaces, no `..`) because older rsync/ssh shells split or execute anything else. Two paths with the same basename are refused.
2. **Pull** over SSH with rsync (read-only on the board): regular files only (symlinks, devices, FIFOs and `.git/` are not copied), files over the size cap are skipped and listed.
3. **Filter** (case-insensitive, also for directories at the root of a given path):
   - *firmware/proprietary*: `rbp*`, `*.UPD`, `*.iso`, `*.img*`, `rootfs*`, `*.cramfs`, `*.pdb`, core dumps, `gui/`, `rbx3-run/`, `rx3-rootfs/`. Only sha256, size and path go in the manifest; `--allow` can never override this.
   - *credentials*: `.ssh/`, `.gnupg/`, `.config/gh/`, `.aws/`, `.docker/`, `.kube/`, `system-connections/`, wpa configs, `*.pem|key|p12|pfx|ppk|kdbx|nmconnection`, `id_*`, `.env*`, `.netrc`, `.credentials.json`, `.claude.json`. Nothing is recorded, not even names or hashes (SSIDs and low-entropy secrets).
   - *binaries and archives* (NUL bytes or stray control characters, or `tar/zip/gz/xz/squashfs/deb/sqlite...`): not stored, hash in the manifest, unless allowed with `--allow`.
4. **Scan** what is left, binary-safe and fail-closed. Private-key headers, GitHub/Anthropic/AWS/Google/Slack/npm/Tailscale tokens and `psk=` abort with exit 3. Weaker `password=`/`token:` lines only warn.
5. **Commit** under `backups/orangepi/<date>-<label>/` (`files/` + `MANIFEST.md`) on a new branch created from the repo's default branch **without tracking it**, using a temporary worktree. Files are added with `git add -f` and the staged count is compared with what was pulled (exit 6 on mismatch), so a nested `.gitignore` cannot silently drop files.
6. **Confirm and push** explicitly to `refs/heads/<branch>`. The question names the origin URL. `--yes` is ignored when credential-like lines were found or binaries were allowed.

Exit codes: 2 validation/usage, 3 secret found or scan error, 4 branch exists, 5 nothing left to commit, 6 integrity mismatch. A branch that did not reach a commit is deleted automatically.

## Rules
- Run it only on the Mac. Do not install GitHub tokens on the board.
- Never push backups to the default branch; merge is a separate, deliberate step.
- Proprietary firmware and derived binaries (`rbp`, patched or not) are never stored; keep their hashes in the manifest.
- Own-built shims, scripts, configs, logs and notes are fine, but review the summary before answering `y`.
- Add a `.gitignore` to the target repo mirroring this repo's firmware/keys section as a second line of defense (the target currently has none).
- If Claude drives the Mac session, it must show the command and wait for your confirmation before the push step.

## Restore
`git fetch origin backup/orangepi-<date>-<label>` then `git checkout` it, and `scp -r backups/orangepi/<date>-<label>/files/<dir> orangepi:~/restore/`. Firmware must come from your own extraction, matched against the hashes in the manifest.

## Tested
Local mode (`OPI_HOST=local`, fake source, bare local remote), 27 checks, after an independent security review reproduced 3 critical/high leaks in the first version. Covered: blocklist for directories at the path root (`.ssh`, `system-connections`, `rbx3-run`, `gui`), renamed ELF and binary private keys, command injection and spaces in paths, newline file names, nested `.git` and `.gitignore`, missing `origin/HEAD`, branch not tracking the base, nothing-left (exit 5, no orphan branch), duplicate basenames, symlinks, wrong target repo, `--allow` and forced confirmation, firmware never allowed, `psk=` abort, push leaving the default branch untouched.
Not yet verified: a real SSH run against the board, bash 3.2 and rsync 2.6.9/openrsync on macOS, a scan failure from unreadable files (tests ran as root), `shellcheck`. Do a first real run with `--no-push` and read `MANIFEST.md` before pushing.

## Backups taken
| Date | Target branch (RX3-Orange-PI-4-LTS) | Sources | Notes |
|---|---|---|---|
| 2026-10-02 | `backup/orangepi-2026-10-02-orangepi4-lts-snapshot-v2` (commit `23708a5`) | `~/rx3-build`, `~/orangepi-rx3.sh`, `~/orangepi-run-test.sh` | 150 files, about 1 MiB, no firmware/credentials found by a name and text scan (not a binary inspection). Made **on the board** (`host alias: local`, paths `/root/...`) with the first, pre-hardening version of the script; later backups must use the current script and the Mac flow. |

Not backed up on purpose: `~/rx3-rootfs` (contains the proprietary `rbp`; keep only hashes, see docs/16), large scan logs, and `~/ref/*` (read-only clones of repos already in git).

### Exception and gaps (2026-10-02)
- The user explicitly chose to run the backup **on the board** and push from it (the board's Claude installed `gh` and authenticated). That bypasses the Mac-only rule and leaves a GitHub token on the board. After the backups, run `gh auth logout` there and revoke the authorization at github.com/settings/applications; use a fine-grained token if board pushes are wanted again.
- The first backup only covered `/root` (`~/rx3-build` and two scripts). The working versions live in `/home/orangepi/` and have **not** been backed up: `RX3-RK3399-DDJ400` (already a git repo; its history is on GitHub as `rbtv` branch `port/rk3399-ddj400`, last commit 2026-09-24), `RX3-RK3399-DDJ400-1024x768`, `RX3-RK3399-DDJ400-1280x800-test-20260921-001223`, `rk3399-source-backup-20260923`, `rx3-promotion-safety-20260922-220958`, `rx3-private`, `rx3-references`, `az-pi4-Orangepi4`, `prepare-stems-pad-control.py`.
- Before backing these up: check `git status`/`git log` in `RX3-RK3399-DDJ400` (the repo may already hold the work); never include `work/` (runtime chroot with firmware files) or `rx3-private`; run one `--no-push` backup per directory and read each `MANIFEST.md`. Backing up an embedded git clone (such as `~/rx3-build`, a modified `xsploit/rx3-pi`) now works because the script skips `.git/`; mind the third-party license/notice when publishing.

