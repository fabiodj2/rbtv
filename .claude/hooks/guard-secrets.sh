#!/usr/bin/env bash
# PreToolUse (Write|Edit|Bash): block touching firmware/keys/credentials (ECC security guide).
# Write/Edit: match the target path only. Bash: block broad `git add` that could stage them.
input=$(cat)
tool=$(printf '%s' "$input" | jq -r '.tool_name // ""')
if [ "$tool" = "Bash" ]; then
  cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // ""')
  if printf '%s' "$cmd" | grep -Eq '(^|[;&|] *)git +add +(-A|--all|\.)( |$)'; then
    echo "[guard] blocked: stage files by name, never 'git add -A/.' (firmware/keys must not be committed)." >&2
    exit 2
  fi
else
  path=$(printf '%s' "$input" | jq -r '.tool_input.file_path // ""')
  if printf '%s' "$path" | grep -Eiq '(\.key|\.pem|\.env|\.env\..*|id_rsa.*|id_ed25519.*|\.upd|rootfs\.cramfs|export\.pdb)$'; then
    echo "[guard] blocked: firmware/keys/credentials must never be written (see .gitignore)." >&2
    exit 2
  fi
fi
exit 0
