#!/bin/sh
# ddj400-start.sh - start/stop the DDJ-400 -> rbp MIDI bridge (Chromebit host).
# Deployed to /home/user/ddj400-start.sh.
#
#   sh /home/user/ddj400-start.sh start      # (default) run in the background
#   sh /home/user/ddj400-start.sh stop
#   sh /home/user/ddj400-start.sh status
#   sh /home/user/ddj400-start.sh sniff      # foreground: log raw MIDI, send nothing
#   sh /home/user/ddj400-start.sh watch      # tail the bridge log
#
# The bridge is normally started automatically by start-rb.sh.
set -u
BIN=${BIN:-/usr/local/bin/ddj400-bridge}
FIFO=/tmp/rb-ctrl.fifo
LOG=/tmp/ddj400.log

running() { pgrep -f '[d]dj400-bridge' >/dev/null 2>&1; }

case "${1:-start}" in
start)
    [ -p "$FIFO" ] || mkfifo "$FIFO" 2>/dev/null
    chmod 666 "$FIFO" 2>/dev/null
    if running; then
        echo "ddj400-bridge already running (pid $(pgrep -f '[d]dj400-bridge' | tr '\n' ' '))"
        exit 0
    fi
    [ -x "$BIN" ] || { echo "not installed: $BIN (run scripts/rb/deploy-ddj400.sh)" >&2; exit 1; }
    : > "$LOG"
    nohup "$BIN" -v </dev/null >>"$LOG" 2>&1 &
    sleep 1
    if running; then
        echo "ddj400-bridge started -> $LOG"
    else
        echo "ddj400-bridge failed to start; log:"; tail -5 "$LOG"
        exit 1
    fi
    ;;
stop)
    if running; then
        pkill -f '[d]dj400-bridge'
        echo "ddj400-bridge stopped"
    else
        echo "ddj400-bridge not running"
    fi
    ;;
restart)
    "$0" stop; sleep 1; exec "$0" start
    ;;
status)
    if running; then
        echo "running: $(pgrep -f '[d]dj400-bridge' | tr '\n' ' ')"
    else
        echo "not running"
    fi
    echo "--- fifo ---"; ls -l "$FIFO" 2>&1
    echo "--- alsa cards ---"; grep -i ddj /proc/asound/cards 2>/dev/null || echo "(no DDJ card)"
    echo "--- midi nodes ---"; ls /dev/snd/midi* 2>/dev/null || echo "(none)"
    echo "--- log tail ---"; tail -5 "$LOG" 2>/dev/null
    ;;
sniff)
    [ -x "$BIN" ] || { echo "not installed: $BIN" >&2; exit 1; }
    echo "sniffing raw MIDI from the DDJ-400 (Ctrl-C to stop)..."
    exec "$BIN" -s
    ;;
watch)
    tail -f "$LOG"
    ;;
*)
    echo "usage: $0 {start|stop|restart|status|sniff|watch}" >&2
    exit 2
    ;;
esac
