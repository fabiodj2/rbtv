#!/bin/sh
set -eu

# O RBP é grande; um único crash pode produzir core de centenas de MB.
ulimit -c 0


REPO=$(cd "$(dirname "$0")/../.." && pwd)
RUNTIME="$REPO/work/runtime/rx3"
TOUCH="$REPO/work/build/host/rx3-touch-bridge"
DDJ="$REPO/work/build/host/ddj400-bridge"
WATCHER="$REPO/scripts/rb/device/usb-watch.sh"
LOGDIR="$REPO/work/tests/integrated-live"

HP_GAIN=${RX3_HP_GAIN_X:-4}
USB_SEG=${USBWATCH_SEG:-/usb}

status()
{
    echo '=== PROCESSOS ==='
    pgrep -af \
        'rbp -a|edb_streamd|rx3-touch-bridge|ddj400-bridge|usb-watch' ||
        true

    echo '=== ÁUDIO ==='
    grep -E \
        'opened real|headphone digital gain|hw_params|writei' \
        "$RUNTIME/tmp/audioshim.log" 2>/dev/null |
        tail -20 || true

    echo '=== KEYSHIM ==='
    grep -E \
        'key manager|crossfader assigned|mixer defaults|fifo open|ERROR' \
        "$RUNTIME/tmp/keyshim.log" 2>/dev/null |
        tail -20 || true

    echo '=== USB ==='
    sudo env \
        RX3_RUNTIME="$RUNTIME" \
        USBWATCH_SEG="$USB_SEG" \
        sh "$WATCHER" status 2>/dev/null || true
}

stop_all()
{
    sudo env RX3_RUNTIME="$RUNTIME" \
        USBWATCH_SEG="$USB_SEG" \
        sh "$WATCHER" stop 2>/dev/null || true

    sudo pkill -TERM -f \
        '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' 2>/dev/null || true

    sudo pkill -TERM -f \
        '^/lib/ld-linux\.so\.3 /usr/bin/edb_streamd$' 2>/dev/null || true

    sudo pkill -TERM -f \
        'work/build/host/ddj400-bridge' 2>/dev/null || true

    sudo pkill -TERM -f \
        'work/build/host/rx3-touch-bridge' 2>/dev/null || true

    sleep 2
}

case "${1:-start}" in
    stop)
        stop_all
        exit 0
        ;;
    status)
        status
        exit 0
        ;;
    start)
        ;;
    restart)
        ;;
    *)
        echo "Uso: $0 start|stop|restart|status" >&2
        exit 2
        ;;
esac

mkdir -p "$LOGDIR"
sudo -v

echo '=== ENCERRANDO INSTÂNCIAS ANTERIORES ==='
stop_all

echo '=== PREPARANDO RUNTIME E MONTAGENS ==='

sudo mkdir -p \
    "$RUNTIME/proc" \
    "$RUNTIME/sys" \
    "$RUNTIME/dev/snd" \
    "$RUNTIME/dev/shm" \
    "$RUNTIME/dev" \
    "$RUNTIME/tmp" \
    "$RUNTIME/media/usb1/sda1"

sudo mountpoint -q "$RUNTIME/proc" ||
    sudo mount --bind /proc "$RUNTIME/proc"

sudo mountpoint -q "$RUNTIME/sys" ||
    sudo mount --bind /sys "$RUNTIME/sys"

sudo mountpoint -q "$RUNTIME/dev/shm" ||
    sudo mount --bind /dev/shm "$RUNTIME/dev/shm"

sudo mountpoint -q "$RUNTIME/dev/snd" ||
    sudo mount --bind /dev/snd "$RUNTIME/dev/snd"

if [ ! -e "$RUNTIME/dev/fb0" ]; then
    sudo mknod "$RUNTIME/dev/fb0" c 29 0
fi

sudo mountpoint -q "$RUNTIME/dev/fb0" ||
    sudo mount --bind /dev/fb0 "$RUNTIME/dev/fb0"

echo '=== PREPARANDO FIFOS ==='

for FIFO in \
    "$RUNTIME/tmp/rb-ctrl.fifo" \
    "$RUNTIME/dev/tsc2007_2-0048" \
    "$RUNTIME/dev/rx3-control"
do
    sudo rm -f "$FIFO"
    sudo mkfifo -m 0666 "$FIFO"
done

sudo rm -f \
    "$RUNTIME/tmp/req_LocalDBServer" \
    "$RUNTIME/tmp/guard_LocalDBServer" \
    "$RUNTIME/tmp/audioshim.log" \
    "$RUNTIME/tmp/keyshim.log"

TOUCH_DEVICE=$(
    find /dev/input/by-id -maxdepth 1 -type l \
        -name '*TouchScreen*event-if00' |
    head -1
)

[ -n "$TOUCH_DEVICE" ] || TOUCH_DEVICE=/dev/input/event2

echo "Touch: $TOUCH_DEVICE"
echo "USB:   $USB_SEG"
echo "Ganho do fone: ${HP_GAIN}x"

if [ -w /sys/class/vtconsole/vtcon1/bind ]; then
    echo 0 | sudo tee \
        /sys/class/vtconsole/vtcon1/bind >/dev/null || true
fi

echo '=== DESABILITANDO LOG DIRECTFB DE ALTA FREQUÊNCIA ==='

sudo rm -f "$RUNTIME/tmp/dfbdig9.log"
sudo ln -s /dev/null "$RUNTIME/tmp/dfbdig9.log"

# fbshim32 registrava cada ioctl e crescia aproximadamente 6 GB por dia.
sudo rm -f "$RUNTIME/tmp/fbshim32.log"
sudo ln -s /dev/null "$RUNTIME/tmp/fbshim32.log"

echo '=== INICIANDO EDB_STREAMD ==='

nohup sudo chroot "$RUNTIME" \
    /lib/ld-linux.so.3 /usr/bin/edb_streamd \
    >"$LOGDIR/edb-streamd.log" 2>&1 </dev/null &

echo '=== INICIANDO TOUCH ==='

nohup sudo env RX3_RUNTIME="$RUNTIME" \
    "$TOUCH" \
    "$TOUCH_DEVICE" \
    "$RUNTIME/dev/tsc2007_2-0048" \
    --fullscreen \
    >"$LOGDIR/touch.log" 2>&1 </dev/null &

echo '=== INICIANDO DDJ-400 ==='

nohup sudo "$DDJ" \
    -v \
    -f "$RUNTIME/tmp/rb-ctrl.fifo" \
    >"$LOGDIR/ddj400-bridge.log" 2>&1 </dev/null &

echo '=== INICIANDO PLAYER ==='

nohup sudo chroot "$RUNTIME" env \
    DFBARGS='system=fbdev,mode=1280x800,pixelformat=ARGB,primary-only,no-hardware' \
    DFB_ROTATE=off \
    RX3_TOUCH_BRIDGE=1 \
    RX3_AUDIO_DEVICE='hw:CARD=DDJ400,DEV=0' \
    RX3_HP_GAIN_X="$HP_GAIN" \
    RX3_KEYSHIFT=1 \
    RX3_STEMS_DIR=/root/pdj/RX3_STEMS \
    LD_PRELOAD=/root/pdj/librx3_core.so:/usr/lib/memshim.so:/usr/lib/fbshim.so:/usr/lib/audioshim.so:/usr/lib/keyshim.so:/usr/lib/netshim.so \
    /lib/ld-linux.so.3 /root/pdj/rbp -a \
    >"$LOGDIR/rbp.log" 2>&1 </dev/null &

sleep 8

echo '=== INICIANDO USB WATCHER ==='

sudo env \
    RX3_RUNTIME="$RUNTIME" \
    USBWATCH_SEG="$USB_SEG" \
    sh "$WATCHER" start

sleep 10
status
