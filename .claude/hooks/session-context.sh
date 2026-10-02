#!/usr/bin/env bash
# SessionStart: surface the newest session note so work resumes where it stopped.
d="$(git rev-parse --show-toplevel 2>/dev/null)/docs/sessions"
last=$(ls -1t "$d"/*.md 2>/dev/null | head -1)
[ -n "$last" ] && echo "Previous session note: ${last#$PWD/} — read it before starting (what worked / failed / left to do)."
exit 0
