#!/usr/bin/env bash
# PreCompact: remind to persist state before context is compacted.
echo "[hook] Context compacting: run /session-handoff first if there is unsaved progress." >&2
exit 0
