#!/bin/sh
# build-rbp.sh — build the Chromebit `rbp` (rekordbox player) from the stock
# XDJ-RX3 v1.20 binary.
#
#   1. PrimeBox's rbp_patch.py            -> rbp-audio      (68 patches:
#      panel/USB/keys/audio/waveform; the Prime GO patch set)
#   2. our patch-rbp-crashguards.py       -> rbp-chromebit  (the getPcController()
#      NULL guard only)
#
# Step 2 is NOT optional on the Chromebit: without it `rbp` SIGSEGVs ~1 s after
# start at pc=0x31df70 / addr=0x9c (the JUCE NetworkMonitor timer dereferencing
# the uninitialised PC-controller singleton).  See docs/04 and docs/07.
#
# NOTE: do NOT use rb2go's patch-rbp-debug.py wholesale.  It also forces
# `playengine::Player::getTotalLength()` to the "no data" sentinel (0x63a44),
# a phone-only workaround that leaves the Chromebit deck with no duration:
# PLAY does nothing and the middle scrolling waveform never renders.  See
# patch-rbp-crashguards.py for the full explanation.
#
# Usage:
#   STOCK=/path/to/XDJRX3/pdj/rbp ./scripts/rb/build-rbp.sh
#
# Stock md5 4f2efcfc0c9e3f539289f863acfddcc6.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
PRIMEBOX=${PRIMEBOX:-$REPO/../PrimeBox}
RBX3=${RBX3:-$REPO/../rbx3-firmware}
STOCK=${STOCK:-$RBX3/XDJRX3/pdj/rbp}
OUTDIR=${OUTDIR:-$REPO/work/rb}
PB=${PB:-$PRIMEBOX/tools/patch-rbp/rbp_patch.py}
GUARDS=$REPO/scripts/rb/patch-rbp-crashguards.py

[ -f "$STOCK" ]  || { echo "stock rbp not found: $STOCK" >&2; exit 1; }
[ -f "$PB" ]     || { echo "PrimeBox patcher not found: $PB" >&2; exit 1; }
[ -f "$GUARDS" ] || { echo "crashguard patcher not found: $GUARDS" >&2; exit 1; }
mkdir -p "$OUTDIR"

echo "== 1. PrimeBox patch set -> rbp-audio =="
python3 "$PB" "$STOCK" -o "$OUTDIR/rbp-audio"

echo "== 2. getPcController crash guard -> rbp-chromebit =="
python3 "$GUARDS" --in "$OUTDIR/rbp-audio" --out "$OUTDIR/rbp-chromebit"

echo
md5sum "$OUTDIR/rbp-audio" "$OUTDIR/rbp-chromebit"
echo "expected rbp-audio     3706c68f7242779d46afa09f35a39acf"
echo "expected rbp-chromebit 18a64bc4d0ffd1cbd35f3a6ea447fca8"
