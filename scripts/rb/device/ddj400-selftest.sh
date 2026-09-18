#!/bin/sh
# ddj400-selftest.sh - validate the DDJ-400 -> rbp control path on the Chromebit
# WITHOUT the controller, by feeding synthetic DDJ-400 MIDI bytes into the
# bridge through a FIFO (the bridge reads whatever -d points at).
#
# Checks, for every control class, that the byte stream survives
#   bridge -> /tmp/rb-ctrl.fifo -> keyshim.so -> IKeyManager::sendKey()
# by looking for the matching lines in /tmp/keyshim.log.
#
# Run on the device:  sh /home/user/ddj400-selftest.sh
set -u
BRIDGE=${BRIDGE:-/usr/local/bin/ddj400-bridge}
FAKE=/tmp/fake-ddj400
FLOG=/tmp/ddj400-selftest.log
KLOG=/tmp/keyshim.log
CTRL_FIFO=/tmp/rb-ctrl.fifo

[ -x "$BRIDGE" ] || { echo "missing $BRIDGE" >&2; exit 1; }
[ -p "$CTRL_FIFO" ] || { echo "missing $CTRL_FIFO (start rbp first)" >&2; exit 1; }
pgrep -f '[k]eyshim' >/dev/null 2>&1 || true   # keyshim is in-process, not a proc

rm -f "$FAKE"; mkfifo "$FAKE"
# keep a writer open so the bridge's poll() does not see POLLHUP on the FIFO
sleep 900 > "$FAKE" &
KEEP=$!

pkill -f '[d]dj400-bridge -v -d '"$FAKE" 2>/dev/null
"$BRIDGE" -v -d "$FAKE" > "$FLOG" 2>&1 &
BPID=$!
sleep 1

# --- synthetic DDJ-400 MIDI (status bytes per the official MIDI list) ---
send() {
    name=$1; shift
    before=$(wc -l < "$KLOG" 2>/dev/null || echo 0)
    printf "$@" > "$FAKE"
    sleep 0.6
    echo "--- $name"
    tail -n +$((before + 1)) "$KLOG" 2>/dev/null | grep -E 'ctrl key=|op=|ch=|param=' | tr '\n' ' '
    echo
}

echo "== bridge -> keyshim selftest =="
# DECK 1/2 transport: PLAY(0x0B) CUE(0x0C) SYNC(0x58) LOOP IN(0x10)
send "deck1 PLAY press+release" '\x90\x0b\x7f\x90\x0b\x00'
send "deck2 CUE press"          '\x91\x0c\x7f'
send "deck1 SYNC press"         '\x90\x58\x7f'
# browser: SELECT (0x41), LOAD1 (0x46), shift+LOAD1 = source (0x68), browse CC
send "browse push (select)"     '\x96\x41\x7f\x96\x41\x00'
send "LOAD deck 2"              '\x96\x47\x7f\x96\x47\x00'
send "shift+LOAD1 (source)"     '\x96\x68\x7f\x96\x68\x00'
send "browse rotate +3"         '\xb6\x40\x01\xb6\x40\x02\xb6\x40\x03'
# mixer: ch1 fader 14-bit at centre (MSB 0x40, LSB 0x00), trim, EQ hi
send "ch1 fader 14-bit centre"  '\xb0\x13\x40\xb0\x33\x00'
send "ch2 trim 14-bit max"      '\xb1\x04\x7f\xb1\x24\x7f'
send "crossfader centre"        '\xb6\x1f\x40\xb6\x3f\x00'
send "ch1 filter knob"          '\xb6\x17\x40'
# tempo sliders + jog
send "deck1 tempo centre"       '\xb0\x00\x40\xb0\x20\x00'
send "deck2 tempo max"          '\xb1\x00\x7f\xb1\x20\x7f'
send "deck1 jog touch + spin"   '\x90\x36\x7f\xb0\x22\x41\xb0\x22\x41\xb0\x22\x41'
send "deck2 jog side spin"      '\xb1\x21\x41\xb1\x21\x41'
# pads: hot cue pad1 (0x00), beat loop pad3 (0x12), beat jump pad8 (0x27)
send "pad1 hot cue"             '\x97\x00\x7f\x97\x00\x00'
send "pad3 beat loop"           '\x97\x12\x7f\x97\x12\x00'
send "pad8 beat jump"           '\x97\x27\x7f\x97\x27\x00'
# deck 2 pads arrive on channel 10 (0-based 9)
send "deck2 pad2 hot cue"       '\x99\x01\x7f\x99\x01\x00'
# beat fx: channel select CH2 (note 0x11), on/off (0x4B), depth
send "beat fx ch2 + on"         '\x94\x11\x7f\x94\x4b\x7f\x94\x4b\x00'
send "beat fx depth"            '\xb4\x02\x7f'

echo
echo "== bridge log (mapping view) =="
grep -E '^  ' "$FLOG" | tail -20

kill "$BPID" 2>/dev/null
kill "$KEEP" 2>/dev/null
rm -f "$FAKE"
echo
echo "done."
