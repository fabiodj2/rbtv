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
BACKUP_REPO=~/rx3-orangepi ~/rbtv/scripts/orangepi/backup-pull.sh <label> '<remote-path>' ['<remote-path>' ...]
# example
BACKUP_REPO=~/rx3-orangepi ~/rbtv/scripts/orangepi/backup-pull.sh rx3-v3-working '~/rx3-handoff' '~/start-rx3.sh'
```
Steps the script performs:
1. **Pull** over SSH with rsync (read-only on the board), files over `OPI_BACKUP_MAX_SIZE` (default 50m) are skipped and listed.
2. **Filter** by name and path: firmware (`rbp*`, `*.UPD`, `*.iso`, `*.img*`, `rootfs*`, `*.cramfs`, `export.pdb`, `gui/`, `rbx3-run/`, `rx3-rootfs/`), keys and credentials (`*.pem`, `*.key`, `id_*`, `.ssh/`, `.env*`, `.netrc`, `.credentials.json`, `.claude.json`, wpa/NetworkManager configs, core dumps). Excluded files are **not stored**; only their sha256, size and path go in the manifest (same idea as the md5 table in docs/16).
3. **Scan** what is left. Strong patterns (private key headers, GitHub/Anthropic/AWS/Slack tokens) abort with exit 3 and nothing is committed. Weaker `password=`/`token:` lines only warn.
4. **Commit** under `backups/orangepi/<date>-<label>/` (`files/` + `MANIFEST.md` with sha256 of every stored file, system, sources, exclusions) on the new branch, using a temporary worktree.
5. **Confirm and push.** It shows the summary and asks `Push ... [y/N]`. `--yes` skips the question, except when credential-like lines were found (always asks). `--no-push` keeps the branch local.

Exit codes: 2 usage, 3 secret found, 4 branch exists, 5 nothing to commit.

## Rules
- Run it only on the Mac. Do not install GitHub tokens on the board.
- Never push backups to the default branch; merge is a separate, deliberate step.
- Proprietary firmware and derived binaries (`rbp`, patched or not) are never stored; keep their hashes in the manifest.
- Own-built shims, scripts, configs, logs and notes are fine, but review the summary before answering `y`.
- Add a `.gitignore` to the target repo mirroring this repo's firmware/keys section as a second line of defense (the target currently has none).
- If Claude drives the Mac session, it must show the command and wait for your confirmation before the push step.

## Restore
`git fetch origin backup/orangepi-<date>-<label>` then `git checkout` it, and `scp -r backups/orangepi/<date>-<label>/files/<dir> orangepi:~/restore/`. Firmware must come from your own extraction, matched against the hashes in the manifest.

## Tested (local mode, fake source)
Clean run, size cap, firmware/key/`.ssh` exclusion, strong-secret abort (exit 3), declined confirmation, push to a bare remote leaving the default branch untouched, duplicate label refused (exit 4), `--yes` ignored on credential warning. Not yet run against the real board or over SSH.
