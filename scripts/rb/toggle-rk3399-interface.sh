#!/bin/sh
set -eu

REQUEST=/run/rx3-interface-toggle.request
STATE=/run/rx3-interface-off
MAIN_SERVICE=rx3-rk3399-ddj400.service

rm -f "$REQUEST"

if pgrep -f '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' \
    >/dev/null 2>&1
then
    echo 'RX3 interface: desligando player e restaurando console'

    pkill -TERM -f \
        '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' \
        2>/dev/null || true

    pkill -TERM -f \
        '^/lib/ld-linux\.so\.3 /usr/bin/edb_streamd$' \
        2>/dev/null || true

    pkill -TERM -f \
        'work/build/host/rx3-touch-bridge' \
        2>/dev/null || true

    sleep 2

    if [ -w /sys/class/vtconsole/vtcon1/bind ]; then
        echo 1 > /sys/class/vtconsole/vtcon1/bind || true
    fi

    if command -v chvt >/dev/null 2>&1; then
        chvt 1 >/dev/null 2>&1 || true
    fi

    : > "$STATE"
    echo 'RX3 interface: OFF'
else
    echo 'RX3 interface: reiniciando runtime completo'
    rm -f "$STATE"

    systemctl restart --no-block "$MAIN_SERVICE"
    echo 'RX3 interface: solicitação ON enviada'
fi
