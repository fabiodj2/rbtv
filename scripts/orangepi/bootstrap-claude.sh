#!/usr/bin/env bash
# Prepare an Orange Pi (or any Debian/Armbian board) to use this repo's Claude Code workflow.
# Idempotent. Installs nothing without sudo confirmation; clones reference repos read-only.
set -euo pipefail

REPO_URL="${RBTV_REPO_URL:-https://github.com/fabiodj2/rbtv}"
BRANCH="${RBTV_BRANCH:-ccr-a8d86acf-yklgcs}"
DEST="${RBTV_DIR:-$HOME/rbtv}"
REF_DIR="${RBTV_REF_DIR:-$HOME/ref}"
REF_REPOS=(fabiodj2/rx3-pi fabiodj2/Rx3-flx4 fabiodj2/RX3-Orange-PI-4-LTS fabiodj2/rblive4-vf)

# Works for public and private repos: prefer an authenticated gh, else plain git without prompting.
export GIT_TERMINAL_PROMPT=0
clone_repo() {  # clone_repo <owner/repo> <dest> [git clone args...]
  local slug="$1" dest="$2"; shift 2
  if command -v gh >/dev/null && gh auth status >/dev/null 2>&1; then
    gh repo clone "$slug" "$dest" -- "$@"
  else
    git clone "$@" "https://github.com/$slug" "$dest" \
      || { echo "clone of $slug failed (private repo? run 'gh auth login' or use a read-only token)" >&2; return 1; }
  fi
}

echo "== bootstrap: repo=$REPO_URL branch=$BRANCH dest=$DEST"

missing=()
for t in git jq gcc bash; do command -v "$t" >/dev/null || missing+=("$t"); done
command -v shellcheck >/dev/null || echo "note: shellcheck not installed (optional, used by hooks)"
if [ "${#missing[@]}" -gt 0 ]; then
  echo "missing tools: ${missing[*]}"
  echo "install with: sudo apt-get install -y ${missing[*]}   (then re-run)"
  exit 1
fi
command -v claude >/dev/null || echo "note: 'claude' not on PATH; install Claude Code first"

if [ -d "$DEST/.git" ]; then
  echo "updating $DEST"
  git -C "$DEST" fetch origin "$BRANCH"
  git -C "$DEST" checkout "$BRANCH"
  git -C "$DEST" merge --ff-only "origin/$BRANCH"
else
  clone_repo "${REPO_URL#https://github.com/}" "$DEST" --branch "$BRANCH"
fi

mkdir -p "$REF_DIR"
for r in "${REF_REPOS[@]}"; do
  d="$REF_DIR/$(basename "$r")"
  if [ -d "$d/.git" ]; then git -C "$d" pull --ff-only -q || echo "warn: could not update $d"
  else clone_repo "$r" "$d" -q --depth 1 || echo "warn: could not clone $r"; fi
done

chmod +x "$DEST"/.claude/hooks/*.sh "$DEST"/.claude/scripts/*.sh "$DEST"/scripts/orangepi/*.sh
if [ ! -e "$DEST/.claude/settings.local.json" ]; then
  cp "$DEST/.claude/settings.board.example.json" "$DEST/.claude/settings.local.json"
  echo "created .claude/settings.local.json from the board template (ask before sudo/dd/mount/systemctl)"
fi

(cd "$DEST" && .claude/scripts/verify.sh | tail -3) || true
cat <<MSG

Done. Next:
  cd $DEST && claude          # agents, rules, skills and hooks load from .claude/
  $DEST/scripts/orangepi/board-triage.sh   # read-only hardware report
Reference repos (read-only copies) are in $REF_DIR.
MSG
