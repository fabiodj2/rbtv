#!/usr/bin/env bash
# Standard path to bring Orange Pi backups into git. Run on the MACBOOK (never on the board):
#   pull over SSH -> filter firmware/keys/credentials -> scan for secrets -> commit on a
#   dedicated branch backup/orangepi-<date>-<label> -> push after you confirm.
#
# Usage: backup-pull.sh [--yes] [--no-push] <label> <remote-path> [<remote-path> ...]
#   e.g. backup-pull.sh rx3-v3-working '~/rx3-handoff' '~/start-rx3.sh'
# Env:   OPI_HOST (ssh alias, default "orangepi"; "local" = read paths from this machine, for tests)
#        OPI_BACKUP_MAX_SIZE (rsync size cap per file, default 50m)
#        BACKUP_REPO (path of the git clone that receives the backup; default: current repo).
#          Standard target: a clone of fabiodj2/RX3-Orange-PI-4-LTS. The script may live in another clone.
# Works with macOS bash 3.2 / rsync 2.6.9 and Linux.
set -euo pipefail

HOST="${OPI_HOST:-orangepi}"
MAX_SIZE="${OPI_BACKUP_MAX_SIZE:-50m}"
ASSUME_YES=0
PUSH=1

while [ $# -gt 0 ]; do
  case "$1" in
    --yes) ASSUME_YES=1; shift ;;
    --no-push) PUSH=0; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -ge 2 ] || { echo "usage: $0 [--yes] [--no-push] <label> <remote-path>..." >&2; exit 2; }
LABEL="$1"; shift
case "$LABEL" in *[!a-z0-9._-]*|"") echo "label must match [a-z0-9._-]+" >&2; exit 2 ;; esac

for t in git rsync; do command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 1; }; done
ROOT="$(git -C "${BACKUP_REPO:-.}" rev-parse --show-toplevel)"
BASE="$(git -C "$ROOT" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null | sed 's|^origin/||')"
BASE="${BASE:-main}"
DATE="$(date +%F)"
BRANCH="backup/orangepi-$DATE-$LABEL"
REL="backups/orangepi/$DATE-$LABEL"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
say() { printf '== %s\n' "$*"; }

STAGE="$(mktemp -d)"; WT=""
cleanup() {
  rm -rf "$STAGE"
  if [ -n "$WT" ] && [ -d "$WT" ]; then git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || true; fi
}
trap cleanup EXIT

# --- 1. pull (read-only on the board) -------------------------------------
say "pulling from $HOST (max file size $MAX_SIZE)"
RSH=(); [ "$HOST" = "local" ] || RSH=(-e ssh)
SKIPPED_BIG="$STAGE.skipped-big"; : > "$SKIPPED_BIG"
for p in "$@"; do
  base="$(basename "$p")"; mkdir -p "$STAGE/data/$base"
  if [ "$HOST" = "local" ]; then src="${p%/}/"; [ -d "$p" ] || { src="$p"; }; else src="$HOST:${p%/}/"; fi
  if [ "$HOST" != "local" ] && ssh "$HOST" "[ -f $p ]" 2>/dev/null; then src="$HOST:$p"; fi
  [ "$HOST" != "local" ] || { [ -d "$p" ] || src="$p"; }
  rsync -a --prune-empty-dirs --max-size="$MAX_SIZE" ${RSH[@]+"${RSH[@]}"} "$src" "$STAGE/data/$base/"
  rsync -an --min-size="$MAX_SIZE" --out-format='%n' ${RSH[@]+"${RSH[@]}"} "$src" "$STAGE/data/$base/" 2>/dev/null \
    | sed "s|^|$base/|" >> "$SKIPPED_BIG" || true
done
UNAME="$( [ "$HOST" = local ] && uname -sm || ssh "$HOST" 'uname -sm' 2>/dev/null || echo unknown )"

# --- 2. filter proprietary firmware / keys / credentials -------------------
say "filtering"
EXCL="$STAGE.excluded"; : > "$EXCL"
blocked() {  # $1 = path relative to STAGE/data (lowercased copy tested)
  local l; l="$(printf '%s' "$1" | tr 'A-Z' 'a-z')"
  case "$l" in
    */.ssh/*|*/system-connections/*|*wpa_supplicant*|*/rbx3-run/*|*/rx3-rootfs/*|*/gui/*) return 0 ;;
  esac
  case "$(basename "$l")" in
    rbp|rbp-*|rbp.*|*.upd|*.iso|*.img|*.img.*|rootfs*|*.cramfs|export.pdb|*.pdb|*.key|*.pem|*.p12|*.kdbx) return 0 ;;
    id_rsa*|id_ed25519*|id_ecdsa*|known_hosts|authorized_keys|.env|.env.*|.netrc|.git-credentials) return 0 ;;
    .credentials.json|.claude.json|shadow|core|core.*|*.core) return 0 ;;
  esac
  return 1
}
( cd "$STAGE/data" && find . -type f | sed 's|^\./||' ) | while IFS= read -r f; do
  if blocked "$f"; then
    printf '%s  %s  %s\n' "$(sha256 "$STAGE/data/$f")" "$(wc -c < "$STAGE/data/$f" | tr -d ' ')" "$f" >> "$EXCL"
    rm -f "$STAGE/data/$f"
  fi
done
find "$STAGE/data" -type d -empty -delete 2>/dev/null || true

# --- 3. secret scan on what is left ----------------------------------------
say "scanning for secrets"
STRONG='BEGIN [A-Z ]*PRIVATE KEY|ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|sk-ant-[A-Za-z0-9_-]{20,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{10,}'
if hits="$(grep -rIlE -- "$STRONG" "$STAGE/data" 2>/dev/null)" && [ -n "$hits" ]; then
  echo "ABORT: secret-looking content found (nothing was committed):" >&2
  printf '%s\n' "$hits" | sed "s|$STAGE/data/|  |" >&2
  echo "Remove it on the board or add the file to the blocklist, then re-run." >&2
  exit 3
fi
WEAK='(api[_-]?key|secret|passw(or)?d|token)[[:space:]]*[=:][[:space:]]*[^[:space:]]{8,}'
WEAKHITS="$(grep -rIliE -- "$WEAK" "$STAGE/data" 2>/dev/null | sed "s|$STAGE/data/||" || true)"

# --- 4. worktree on a dedicated branch, never the base branch ---------------------------
say "preparing branch $BRANCH"
git -C "$ROOT" fetch -q origin "$BASE"
if git -C "$ROOT" show-ref --verify --quiet "refs/heads/$BRANCH" || git -C "$ROOT" ls-remote --exit-code --heads origin "$BRANCH" >/dev/null 2>&1; then
  echo "branch $BRANCH already exists; pick another label" >&2; exit 4
fi
WT="$(mktemp -d)"; rmdir "$WT"
git -C "$ROOT" worktree add -q -b "$BRANCH" "$WT" "origin/$BASE"
mkdir -p "$WT/$REL"
rsync -a "$STAGE/data/" "$WT/$REL/files/"
{
  echo "# Orange Pi backup $DATE-$LABEL"
  echo
  echo "- host alias: $HOST   system: $UNAME"
  echo "- sources: $*"
  echo "- per-file size cap: $MAX_SIZE; firmware, keys and credentials are excluded by name (see below)"
  echo
  echo "## Files (sha256)"; echo '```'
  ( cd "$WT/$REL/files" && find . -type f | sort | while IFS= read -r f; do printf '%s  %s\n' "$(sha256 "$f")" "$f"; done )
  echo '```'
  echo; echo "## Excluded, not stored (sha256  bytes  path)"; echo '```'; cat "$EXCL"; echo '```'
  echo; echo "## Skipped for size (> $MAX_SIZE)"; echo '```'; cat "$SKIPPED_BIG"; echo '```'
} > "$WT/$REL/MANIFEST.md"

git -C "$WT" add -- "$REL"
NFILES="$(git -C "$WT" diff --cached --name-only | wc -l | tr -d ' ')"
[ "$NFILES" -gt 0 ] || { echo "nothing to commit after filtering" >&2; exit 5; }

say "summary"
git -C "$WT" diff --cached --stat | tail -15
echo "files staged: $NFILES   excluded: $(wc -l < "$EXCL" | tr -d ' ')   skipped for size: $(wc -l < "$SKIPPED_BIG" | tr -d ' ')"
if [ -n "$WEAKHITS" ]; then
  echo "WARNING: lines that look like credentials (review before pushing):"
  printf '%s\n' "$WEAKHITS" | sed 's/^/  /'
  [ "$ASSUME_YES" -eq 0 ] || echo "(--yes ignored: credential-like lines always need an interactive confirmation)"
  ASSUME_YES=0
fi

git -C "$WT" commit -q -m "backup(orangepi): $LABEL ($DATE)

Pulled from the board by scripts/orangepi/backup-pull.sh. Firmware, keys and
credentials excluded; see $REL/MANIFEST.md."
say "committed on $BRANCH"

# --- 5. push after confirmation --------------------------------------------
if [ "$PUSH" -eq 0 ]; then
  echo "--no-push: branch kept locally. Push later: git push -u origin $BRANCH"
  git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || true; WT=""
  exit 0
fi
if [ "$ASSUME_YES" -ne 1 ]; then
  printf 'Push %s to origin? [y/N] ' "$BRANCH"; read -r ans || ans=n
  case "$ans" in y|Y|yes) ;; *) echo "not pushed. Branch kept: $BRANCH"; git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || true; WT=""; exit 0 ;; esac
fi
git -C "$WT" push -u origin "$BRANCH"
echo "pushed: $BRANCH"
