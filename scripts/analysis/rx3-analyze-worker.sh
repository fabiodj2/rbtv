#!/bin/sh
# rx3-analyze-worker.sh -- one Tier-1 worker: blocks on the shared FIFO,
# runs rx3-analyze.sh for each path it receives. Run 4 of these (one per
# A53 core worth of budget) via the rx3-analyzer@.service template --
# measured to scale with zero penalty at that count (docs/17).
set -eu

QUEUE="${RX3_ANALYZE_QUEUE:-/run/rx3-analyze/queue.fifo}"
OUTDIR="${RX3_ANALYZE_OUTDIR:-/tmp/rx3-analysis}"
SELF="$(dirname "$0")/rx3-analyze.sh"

mkdir -p "$(dirname "$QUEUE")"
[ -p "$QUEUE" ] || mkfifo -m 666 "$QUEUE"

while true; do
    if IFS=$(printf '\t') read -r path id < "$QUEUE"; then
        [ -n "${path:-}" ] || continue
        "$SELF" "$path" "$id" "$OUTDIR" || \
            echo "rx3-analyze-worker: failed on $path" >&2
    fi
done
