#!/usr/bin/env bash
# PreToolUse (Write): new .md files belong in docs/ (README/CLAUDE/LICENSE/NOTICE excepted).
f=$(jq -r '.tool_input.file_path // ""')
case "$f" in
  *.md) case "$f" in */docs/*|*/.claude/*|*/README.md|*/CLAUDE.md|*/NOTICE.md) ;;
    *) echo "[hook] new .md outside docs/: $f — put notes in docs/ (or docs/sessions/)" >&2; exit 2 ;; esac ;;
esac
exit 0
