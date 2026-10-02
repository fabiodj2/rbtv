#!/usr/bin/env bash
# Standard path to bring Orange Pi backups into git. Run on the MACBOOK (never on the board):
#   pull over SSH -> filter firmware/binaries/credentials -> scan for secrets -> commit on a
#   dedicated branch backup/orangepi-<date>-<label> -> push only after you confirm.
#
# Usage: BACKUP_REPO=<clone> backup-pull.sh [--yes] [--no-push] [--allow <name-glob>]... <label> <remote-path>...
#   e.g. BACKUP_REPO=~/rx3-orangepi backup-pull.sh rx3-v3-working '~/rx3-handoff' '~/start-rx3.sh'
# Env:   BACKUP_REPO         REQUIRED. Git clone that receives the backup (standard: RX3-Orange-PI-4-LTS).
#        BACKUP_EXPECT_REPO  substring the origin URL must contain (default rx3-orange-pi-4-lts; empty disables).
#        OPI_HOST            ssh alias (default "orangepi"; "local" reads paths on this machine, for tests).
#        OPI_BACKUP_MAX_SIZE rsync per-file size cap (default 50m).
# Binaries and archives are NOT stored by default (only sha256 in the manifest); --allow stores files whose
# basename matches the glob (for your own shims) and forces an interactive confirmation.
# Remote paths are limited to [A-Za-z0-9._/~+-] (no spaces) so they are safe for old rsync/ssh shells.
# Exit codes: 2 usage/validation, 3 secret or scan error, 4 branch exists, 5 nothing to commit, 6 integrity.
# Compatible with macOS bash 3.2 and rsync 2.6.9/openrsync (uses only -rtp, --max-size, --min-size, --prune-empty-dirs).
set -euo pipefail
export LC_ALL=C

HOST="${OPI_HOST:-orangepi}"
MAX_SIZE="${OPI_BACKUP_MAX_SIZE:-50m}"
ASSUME_YES=0
PUSH=1
ALLOW=()

die() { echo "$*" >&2; exit "${CODE:-2}"; }
say() { printf '== %s\n' "$*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --yes) ASSUME_YES=1; shift ;;
    --no-push) PUSH=0; shift ;;
    --allow) [ $# -ge 2 ] || die "--allow needs a glob"; ALLOW+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    --) shift; break ;;
    -*) die "unknown option: $1" ;;
    *) break ;;
  esac
done
[ $# -ge 2 ] || die "usage: BACKUP_REPO=<clone> $0 [--yes] [--no-push] [--allow glob] <label> <remote-path>..."
LABEL="$1"; shift

# --- validation (everything user-supplied) ----------------------------------
case "$LABEL" in *[!a-z0-9._-]*|""|.*|*..*|*.lock) die "label must match [a-z0-9._-]+ (no leading dot, '..' or .lock)" ;; esac
case "$HOST" in -*|*[!A-Za-z0-9._:%@-]*|"") die "invalid OPI_HOST: $HOST" ;; esac
for p in "$@"; do
  case "$p" in *[!A-Za-z0-9._/~+-]*|"") die "unsafe remote path (allowed: A-Za-z0-9._/~+-): $p" ;; esac
  case "$p" in *..*) die "path may not contain '..': $p" ;; esac
done
for t in git rsync find; do command -v "$t" >/dev/null || die "missing tool: $t"; done
[ -n "${BACKUP_REPO:-}" ] || die "BACKUP_REPO is required (clone of the backup target repo)"

ROOT="$(git -C "$BACKUP_REPO" rev-parse --show-toplevel)"
URL="$(git -C "$ROOT" remote get-url origin)"
SHOWURL="$(printf '%s' "$URL" | sed 's|//[^@/]*@|//|')"
EXPECT="${BACKUP_EXPECT_REPO-rx3-orange-pi-4-lts}"
if [ -n "$EXPECT" ]; then
  case "$(printf '%s' "$URL" | tr 'A-Z' 'a-z')" in
    *"$EXPECT"*) ;;
    *) die "origin of $ROOT is $SHOWURL, expected it to contain '$EXPECT' (set BACKUP_EXPECT_REPO= to override)" ;;
  esac
fi
BASE="$(git -C "$ROOT" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null || true)"
BASE="${BASE#origin/}"; BASE="${BASE:-main}"
DATE="$(date +%F)"
BRANCH="backup/orangepi-$DATE-$LABEL"
REL="backups/orangepi/$DATE-$LABEL"
git check-ref-format "refs/heads/$BRANCH" || die "invalid branch name: $BRANCH"
[ "$BRANCH" != "$BASE" ] || die "refusing to use the base branch"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

STAGE="$(mktemp -d)"; WT=""; BRANCH_CREATED=0; COMMITTED=0
cleanup() {
  local rc=$?
  [ -z "$WT" ] || git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || true
  if [ "$BRANCH_CREATED" -eq 1 ] && [ "$COMMITTED" -eq 0 ]; then
    git -C "$ROOT" branch -D "$BRANCH" >/dev/null 2>&1 || true
  fi
  rm -rf "$STAGE"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
mkdir -p "$STAGE/data"
EXCL="$STAGE/excluded.txt"; SKIPPED="$STAGE/skipped-big.txt"; ALLOWED="$STAGE/allowed.txt"
: > "$EXCL"; : > "$SKIPPED"; : > "$ALLOWED"

# --- 1. pull (read-only on the board) ---------------------------------------
say "pulling from $HOST (max file size $MAX_SIZE; symlinks, devices and .git/ are not copied)"
RSH=(); [ "$HOST" = "local" ] || RSH=(-e "ssh -o ConnectTimeout=10")
SEEN="|"
for p in "$@"; do
  base="$(basename "$p")"
  case "$SEEN" in *"|$base|"*) die "two paths share the basename '$base'; pass them in separate backups" ;; esac
  SEEN="$SEEN$base|"
  if [ "$HOST" = "local" ]; then isdir=0; [ -d "$p" ] && isdir=1
  else isdir=0; ssh -o ConnectTimeout=10 -- "$HOST" "[ -d $p ]" 2>/dev/null && isdir=1 || true; fi
  if [ "$isdir" -eq 1 ]; then
    dest="$STAGE/data/$base/"; srcp="${p%/}/"
  else
    dest="$STAGE/data/_files/"; srcp="$p"
  fi
  mkdir -p "$dest"
  if [ "$HOST" = "local" ]; then src="$srcp"; else src="$HOST:$srcp"; fi
  rsync -rtp --prune-empty-dirs --max-size="$MAX_SIZE" --exclude='.git/' ${RSH[@]+"${RSH[@]}"} "$src" "$dest"
  if big="$(rsync -rn --min-size="$MAX_SIZE" --out-format='%n' ${RSH[@]+"${RSH[@]}"} "$src" "$dest" 2>/dev/null)"; then
    printf '%s\n' "$big" | grep -v '/$' | sed "/^\$/d; s|^|$base/|" >> "$SKIPPED" || true
  else
    echo "(could not list files over $MAX_SIZE: rsync lacks --min-size/--out-format)" >> "$SKIPPED"
  fi
done
UNAME="$( [ "$HOST" = local ] && uname -sm || ssh -o ConnectTimeout=10 -- "$HOST" 'uname -sm' 2>/dev/null || echo unknown )"

# --- 2. filter firmware / binaries / credentials -----------------------------
say "filtering"
[ -z "$(find "$STAGE/data" -name "$(printf '*\n*')" -print -quit)" ] || die "file names containing a newline are not supported"

CTRL="$(printf '\001-\010\016-\032\034-\037')"
classify() {  # $1 = path relative to data/ ; prints fw | cred | bin | (nothing = keep)
  local rel="$1" l base f g isbin=0
  l="/$(printf '%s' "$rel" | tr 'A-Z' 'a-z')"; base="${l##*/}"
  case "$l" in
    */.ssh/*|*/.gnupg/*|*/.config/gh/*|*/.docker/*|*/.aws/*|*/.kube/*|*/system-connections/*|*wpa_supplicant*) echo cred; return ;;
    */rbx3-run/*|*/rx3-rootfs/*|*/gui/*) echo fw; return ;;
  esac
  case "$base" in
    id_rsa*|id_ed25519*|id_ecdsa*|id_dsa*|known_hosts|authorized_keys|.env|.env.*|.netrc|.git-credentials|.credentials.json|.claude.json|shadow|*.pem|*.key|*.p12|*.pfx|*.ppk|*.kdbx|*.nmconnection) echo cred; return ;;
    rbp*|*.upd|*.iso|*.img|*.img.*|rootfs*|*.cramfs|export.pdb|*.pdb|core|core.*|*.core) echo fw; return ;;
  esac
  f="$STAGE/data/$rel"
  case "$base" in
    *.tar|*.tar.*|*.tgz|*.tbz2|*.txz|*.zip|*.gz|*.bz2|*.xz|*.zst|*.7z|*.rar|*.squashfs|*.sqsh|*.deb|*.rpm|*.apk|*.jar|*.cpio|*.sqlite|*.sqlite3|*.db) isbin=1 ;;
  esac
  # binary = has a NUL byte, or control chars other than \t \n \r \033 (colored logs stay text)
  if [ "$isbin" -eq 0 ] && [ -s "$f" ]; then
    if ! grep -qI '' "$f" 2>/dev/null || grep -qa "[$CTRL]" "$f" 2>/dev/null; then isbin=1; fi
  fi
  if [ "$isbin" -eq 1 ]; then
    for g in ${ALLOW[@]+"${ALLOW[@]}"}; do
      case "${rel##*/}" in $g) printf '%s\n' "$rel" >> "$ALLOWED"; return ;; esac
    done
    echo bin
  fi
}

NCRED=0; NFW=0; NBIN=0
while IFS= read -r -d '' f; do
  f="${f#./}"
  cls="$(classify "$f")"
  [ -n "$cls" ] || continue
  case "$cls" in
    cred) NCRED=$((NCRED+1)); printf '(credential file, name and hash withheld)\n' >> "$EXCL" ;;
    fw)   NFW=$((NFW+1));   printf '%s  %s  %s  [firmware/proprietary]\n' "$(sha256 "$STAGE/data/$f")" "$(wc -c < "$STAGE/data/$f" | tr -d ' ')" "$f" >> "$EXCL" ;;
    bin)  NBIN=$((NBIN+1)); printf '%s  %s  %s  [binary/archive]\n' "$(sha256 "$STAGE/data/$f")" "$(wc -c < "$STAGE/data/$f" | tr -d ' ')" "$f" >> "$EXCL" ;;
  esac
  rm -f "$STAGE/data/$f"
done < <(cd "$STAGE/data" && find . -type f -print0)
find "$STAGE/data" -type d -empty -delete 2>/dev/null || true
mkdir -p "$STAGE/data"
NKEEP="$(find "$STAGE/data" -type f | wc -l | tr -d ' ')"
[ "$NKEEP" -gt 0 ] || { CODE=5; die "nothing left to commit after filtering (excluded: fw=$NFW bin=$NBIN cred=$NCRED)"; }

# --- 3. secret scan on what is left (binary-safe, fails closed) --------------
say "scanning for secrets"
STRONG='BEGIN [A-Z ]*PRIVATE KEY|PuTTY-User-Key-File|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|sk-ant-[A-Za-z0-9_-]{20,}|AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|xox[baprs]-[A-Za-z0-9-]{10,}|npm_[A-Za-z0-9]{30,}|tskey-[A-Za-z0-9-]{10,}|(^|[^A-Za-z])(wpa-)?psk[[:space:]]*=[[:space:]]*[^[:space:]]{6,}'
rc=0; hits="$(grep -rlaE -- "$STRONG" "$STAGE/data" 2>/dev/null)" || rc=$?
if [ "$rc" -gt 1 ]; then CODE=3; die "ABORT: secret scan failed (grep rc=$rc, unreadable files?); nothing committed"; fi
if [ -n "$hits" ]; then
  echo "ABORT: secret-looking content found (nothing was committed):" >&2
  printf '%s\n' "$hits" | sed "s|$STAGE/data/|  |" >&2
  CODE=3; die "Remove it on the board or add the file to the blocklist, then re-run."
fi
WEAK='(api[_-]?key|secret|passw(or)?d|token)[[:space:]]*[=:][[:space:]]*[^[:space:]]{8,}'
WEAKHITS="$(grep -rlaiE -- "$WEAK" "$STAGE/data" 2>/dev/null | sed "s|$STAGE/data/||" || true)"

# --- 4. worktree on a dedicated branch (never the base, never tracking it) ---
say "preparing branch $BRANCH in $SHOWURL"
git -C "$ROOT" fetch -q origin "$BASE"
if git -C "$ROOT" show-ref --verify --quiet "refs/heads/$BRANCH" || git -C "$ROOT" ls-remote --exit-code --heads origin "$BRANCH" >/dev/null 2>&1; then
  CODE=4; die "branch $BRANCH already exists; pick another label"
fi
WT="$(mktemp -d)"; rmdir "$WT"
git -C "$ROOT" -c branch.autoSetupMerge=false worktree add -q -b "$BRANCH" "$WT" "origin/$BASE"
BRANCH_CREATED=1
mkdir -p "$WT/$REL"
rsync -rt "$STAGE/data/" "$WT/$REL/files/"
{
  echo "# Orange Pi backup $DATE-$LABEL"
  echo
  echo "- host alias: $HOST   system: $UNAME"
  echo "- sources: $*"
  echo "- per-file size cap: $MAX_SIZE. Firmware, binaries/archives and credentials are not stored (see below)."
  echo "- excluded: firmware=$NFW binaries/archives=$NBIN credentials=$NCRED"
  echo
  echo "## Stored files (sha256)"; echo '```'
  ( cd "$WT/$REL/files" && find . -type f | sort | while IFS= read -r f; do printf '%s  %s\n' "$(sha256 "$f")" "$f"; done )
  echo '```'
  echo; echo "## Allowed binaries/archives (--allow)"; echo '```'; cat "$ALLOWED"; echo '```'
  echo; echo "## Excluded, not stored"; echo '```'; cat "$EXCL"; echo '```'
  echo; echo "## Skipped for size (> $MAX_SIZE)"; echo '```'; cat "$SKIPPED"; echo '```'
} > "$WT/$REL/MANIFEST.md"

# -f: nested/target .gitignore must not silently drop files; verify the count afterwards.
git -C "$WT" add -f -- "$REL"
GOT="$(git -C "$WT" diff --cached --name-only -- "$REL/files" | wc -l | tr -d ' ')"
[ "$GOT" -eq "$NKEEP" ] || { CODE=6; die "integrity check failed: expected $NKEEP files staged, got $GOT"; }

say "summary"
git -C "$WT" diff --cached --stat | tail -15
echo "files staged: $GOT   excluded: firmware=$NFW binaries=$NBIN credentials=$NCRED   skipped for size: $(grep -c . "$SKIPPED" || true)"
if [ -s "$ALLOWED" ]; then
  echo "ALLOWED binaries/archives stored (review!):"; sed 's/^/  /' "$ALLOWED"; ASSUME_YES=0
fi
if [ -n "$WEAKHITS" ]; then
  echo "WARNING: lines that look like credentials (review before pushing):"
  printf '%s\n' "$WEAKHITS" | sed 's/^/  /'
  [ "$ASSUME_YES" -eq 0 ] || echo "(--yes ignored: credential-like lines always need an interactive confirmation)"
  ASSUME_YES=0
fi

git -C "$WT" commit -q -m "backup(orangepi): $LABEL ($DATE)

Pulled from the board by scripts/orangepi/backup-pull.sh. Firmware, binaries
and credentials excluded; see $REL/MANIFEST.md."
COMMITTED=1
say "committed on $BRANCH"

# --- 5. push after confirmation ---------------------------------------------
if [ "$PUSH" -eq 0 ]; then
  echo "--no-push: branch kept locally in $ROOT. Push later: git push -u origin $BRANCH:refs/heads/$BRANCH"
  exit 0
fi
if [ "$ASSUME_YES" -ne 1 ]; then
  printf 'Push %s to %s? [y/N] ' "$BRANCH" "$SHOWURL"; read -r ans || ans=n
  case "$ans" in y|Y|yes) ;; *) echo "not pushed. Branch kept: $BRANCH"; exit 0 ;; esac
fi
git -C "$WT" push -u origin "$BRANCH:refs/heads/$BRANCH"
echo "pushed: $BRANCH -> $SHOWURL"
