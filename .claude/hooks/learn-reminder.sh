#!/usr/bin/env bash
# Stop hook (ECC continuous-learning, project-scoped): once per long session, nudge to capture learnings.
# Runs at session end, not per prompt, so it adds no latency. Reads only the message count.
min=10
t=$(jq -r '.transcript_path // empty' 2>/dev/null)
[ -n "$t" ] && [ -f "$t" ] || exit 0
n=$(grep -c '"type":"user"' "$t" 2>/dev/null || true)
[ "${n:-0}" -ge "$min" ] || exit 0
echo "[learn] Session has $n user messages: if you solved a non-trivial problem, run /continuous-learning to save it under .claude/skills/learned/." >&2
exit 0
