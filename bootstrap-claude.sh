#!/usr/bin/env bash
# Prepare an Orange Pi (or any Debian/Armbian board) to use this repo's Claude Code workflow.
# Idempotent. Installs nothing without sudo confirmation; clones reference repos read-only.
set -euo pipefail

REPO_URL="${RBTV_REPO_URL:-https://github.com/fabiodj2/rbtv}"
BRANCH="${RBTV_BRANCH:-ccr-a8d86acf-yklgcs}"
DEST="${RBTV_DIR:-$HOME/rbtv}"
REF_DIR="${RBTV_REF_DIR:-$HOME/ref}"
REF_REPOS=(fabiodj2/rx3-pi fabiodj2/Rx3-flx4 fabiodj2/RX3-Orange-PI-4-LTS fabiodj2/rblive4-vf)

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
  git clone --branch "$BRANCH" "$REPO_URL" "$DEST"
fi

mkdir -p "$REF_DIR"
for r in "${REF_REPOS[@]}"; do
  d="$REF_DIR/$(basename "$r")"
  if [ -d "$d/.git" ]; then git -C "$d" pull --ff-only -q || echo "warn: could not update $d"
  else git clone -q --depth 1 "https://github.com/$r" "$d" || echo "warn: could not clone $r"; fi
done

chmod +x "$DEST"/.claude/hooks/*.sh "$DEST"/.claude/scripts/*.sh "$DEST"/scripts/orangepi/*.sh
if [ ! -e "$DEST/.claude/settings.local.json" ]; then
  cp "$DEST/.claude/settings.board.example.json" "$DEST/.claude/settings.local.json"
  echo "created .claude/settings.local.json from the board template (ask before sudo/dd/mount/systemctl)"
fi

"$DEST/.claude/scripts/verify.sh" | tail -3 || true
cat <<MSG

Done. Next:
  cd $DEST && claude          # agents, rules, skills and hooks load from .claude/
  $DEST/scripts/orangepi/board-triage.sh   # read-only hardware report
Reference repos (read-only copies) are in $REF_DIR.
MSG
