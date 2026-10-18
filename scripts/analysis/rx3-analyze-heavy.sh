#!/bin/sh
# rx3-analyze-heavy.sh -- Tier 2 (heavy): MFCC + chroma + beat + structural
# segmentation, via librosa. MEASURED cost: 372s pinned to one A53, 80s on
# whatever core the scheduler picks (usually an A72) -- see docs/17 for the
# full numbers, including the OOM kill this tier triggered under naive
# parallel use.
#
# THIS SCRIPT IS NEVER AUTO-TRIGGERED. It is meant to be invoked by an
# operator action (one track, on demand), always through the systemd unit
# that carries the memory ceiling and OOM priority below -- never run it
# bare in a shell on a unit that is also playing live.
#
# Usage: rx3-analyze-heavy.sh <audio-file> <track-id> [output-dir]
set -eu

AUDIO="$1"
TRACK_ID="$2"
OUTDIR="${3:-/tmp/rx3-analysis}"
LOCKFILE="${RX3_ANALYZE_HEAVY_LOCK:-/run/rx3-analyze/heavy.lock}"

mkdir -p "$OUTDIR" "$(dirname "$LOCKFILE")"
OUT="$OUTDIR/$TRACK_ID.heavy.json"

if [ ! -f "$AUDIO" ]; then
    echo "rx3-analyze-heavy: $AUDIO not found" >&2
    exit 1
fi

exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "rx3-analyze-heavy: another heavy job is already running, refusing to start a second one" >&2
    exit 1
fi

PY="$(dirname "$0")/heavy_analysis.py"
RESULT=$(python3 "$PY" "$AUDIO")
STATUS=$?
if [ "$STATUS" -ne 0 ]; then
    echo "rx3-analyze-heavy: analysis failed for $AUDIO" >&2
    exit 1
fi

cat >"$OUT" <<EOF
{
  "track_id": "$TRACK_ID",
  "source": "$AUDIO",
  "analyzer": "rx3-analyze-tier2-librosa-v1",
  "analyzed_at": "$(date -Iseconds)",
  "raw": "$RESULT"
}
EOF

echo "rx3-analyze-heavy: $TRACK_ID done -> $OUT" >&2
