#!/bin/sh
# press-key.sh — inject an XDJ-RX3 button into a running rbp via the keyshim
# FIFO.  Run ON the Chromebit (root), with rbp running and keyshim.so preloaded.
#
#   sh press-key.sh source
#   sh press-key.sh browse
#   sh press-key.sh load1        # also: load2 play1 play2 cue1 cue2
#   sh press-key.sh 0x0201       # raw keycode
#   sh press-key.sh 0x4311 1     # raw keycode, channel 1
#
# The FIFO record is { int32 key; int32 ch; int32 down } little-endian; a press
# is sent as down=1 followed by down=0 (release).
KEY=${1:-source}
CH=${2:-0}

case "$KEY" in
    source)  K=0x0201 ;;
    browse)  K=0x0202 ;;
    taglist) K=0x0203 ;;
    menu)    K=0x0206 ;;
    link)    K=0x0207 ;;
    rekordbox) K=0x0208 ;;
    usb1)    K=0x0209 ;;
    info)    K=0x020b ;;
    select)  K=0x420c ;;
    back)    K=0x420d ;;
    load1)   K=0x4311 ; CH=${2:-0} ;;
    load2)   K=0x4312 ; CH=${2:-0} ;;
    play1)   K=0x4101 ;;
    play2)   K=0x4102 ;;
    cue1)    K=0x4103 ;;
    cue2)    K=0x4104 ;;
    *)       K=$1 ;;
esac

FIFO=/tmp/rb-keys.fifo
[ -p "$FIFO" ] || { echo "no $FIFO — is rbp running (start-rb.sh)?" >&2; exit 1; }

le32() {
    v=$1
    printf "\\%03o\\%03o\\%03o\\%03o" \
        $(( v        & 255)) $(((v >>  8) & 255)) \
        $(((v >> 16) & 255)) $(((v >> 24) & 255))
}

# Build the whole 12-byte record and emit it in ONE write() so the reader always
# gets a complete record (the shim also accumulates partial reads, but be nice).
rec=$(le32 "$K")$(le32 "$CH")

emit() { printf '%b' "$rec$(le32 "$1")" > "$FIFO"; }

echo "injecting key=$K ch=$CH (press+release) -> $FIFO"
emit 1        # press
sleep 1
emit 0        # release
echo "done. check /tmp/keyshim.log (tail):"
tail -4 /tmp/keyshim.log 2>/dev/null
