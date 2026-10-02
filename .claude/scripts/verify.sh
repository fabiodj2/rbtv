#!/usr/bin/env bash
# Deterministic "code-based grader" for rbtv (ECC verification-loop, adapted). Exit 1 on any FAIL.
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # this repo, never the caller's CWD
fail=0
ok()   { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; fail=1; }
warn() { echo "WARN  $*"; }

# 1. Shell syntax (+ shellcheck when available)
while IFS= read -r f; do
  if head -1 "$f" | grep -q '^#!.*sh'; then
    bash -n "$f" 2>/dev/null && ok "bash -n $f" || bad "bash -n $f"
    if command -v shellcheck >/dev/null; then
      shellcheck -S warning "$f" >/dev/null 2>&1 && ok "shellcheck $f" || warn "shellcheck $f"
    fi
  fi
done < <(git ls-files 'scripts/*' '.claude/hooks/*' '.claude/scripts/*' | grep -v -E '\.(c|h|md|py)$')

# 2. C sources: syntax only (cross headers may be missing, so warn)
if command -v gcc >/dev/null; then
  for f in $(git ls-files '*.c'); do
    gcc -fsyntax-only -w "$f" >/dev/null 2>&1 && ok "gcc -fsyntax-only $f" || warn "gcc syntax $f (cross headers?)"
  done
fi

# 3. Nothing sensitive tracked
if git ls-files | grep -E -i '(\.key|\.pem|^\.env|id_rsa|id_ed25519|\.upd|rootfs\.cramfs|export\.pdb|\.img(\.|$))' | grep .; then
  bad "sensitive/firmware files are tracked"
else ok "no sensitive files tracked"; fi

# 4. Harness config sanity
jq -e . .claude/settings.json >/dev/null 2>&1 && ok "settings.json valid" || bad "settings.json invalid"
for h in $(jq -r '.. | .command? // empty' .claude/settings.json); do
  [ -x "$h" ] && ok "hook executable $h" || bad "hook missing/not executable $h"
done

# 5. Relative markdown links resolve
while IFS= read -r md; do
  d=$(dirname "$md")
  grep -o '\]([^)#:]*\.md[^)]*)' "$md" | sed 's/^](//; s/)$//; s/#.*//' | while read -r l; do
    [ -e "$d/$l" ] || { echo "FAIL  broken link $md -> $l"; echo x > /tmp/.rbtv_verify_fail; }
  done
done < <(git ls-files '*.md')
if [ -e /tmp/.rbtv_verify_fail ]; then rm -f /tmp/.rbtv_verify_fail; fail=1; else ok "markdown links"; fi

echo; [ $fail -eq 0 ] && echo "VERIFY: PASS" || echo "VERIFY: FAIL"
exit $fail
