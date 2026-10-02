#!/usr/bin/env bash
# PostToolUse (Write|Edit): syntax-check shell scripts right after edits.
f=$(jq -r '.tool_input.file_path // ""')
case "$f" in
  *.sh|*/scripts/*) head -1 "$f" 2>/dev/null | grep -q '^#!.*sh' || exit 0
    bash -n "$f" 2>&1 >/dev/null || { echo "[hook] bash -n failed: $f" >&2; exit 2; }
    command -v shellcheck >/dev/null && shellcheck -S warning "$f" >&2 || true ;;
esac
exit 0
