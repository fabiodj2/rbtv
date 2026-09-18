#!/bin/sh
# deploy-keyboard.sh — install physical-keyboard control (rbkeyd) on a Chromebit.
#
#   * rbkeyd    built natively on the device (evdev, no dependencies)
#               -> /usr/local/bin/rbkeyd
#   * fakekbd   uinput test tool (inject keys without a real keyboard)
#               -> /usr/local/bin/fakekbd
#   * rbkeyd.service  systemd unit so a keyboard works as soon as it is
#               plugged in (Restart=always, waits for the FIFOs)
#
# Usage:
#   PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-keyboard.sh
#   SERVICE=0 ...   # do not install/enable the systemd unit
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=${HOST:-root@chromebit.local}
PASS=${PASS:?set PASS to the device password}
SERVICE=${SERVICE:-1}
S="sshpass -p $PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $HOST"

echo "== upload sources =="
$S "cat > /tmp/rbkeyd.c" < "$REPO/scripts/rb/device/rbkeyd.c"
$S "cat > /tmp/fakekbd.c" < "$REPO/scripts/rb/device/fakekbd.c"

echo "== build on the device =="
$S "set -e
    command -v gcc >/dev/null || { echo 'no gcc on the device' >&2; exit 1; }
    gcc -O2 -Wall -Wextra -o /tmp/rbkeyd  /tmp/rbkeyd.c
    gcc -O2 -Wall -Wextra -o /tmp/fakekbd /tmp/fakekbd.c
    install -m 755 /tmp/rbkeyd  /usr/local/bin/rbkeyd
    install -m 755 /tmp/fakekbd /usr/local/bin/fakekbd
    echo -n 'installed rbkeyd:  '; md5sum /usr/local/bin/rbkeyd  | cut -d' ' -f1
    echo -n 'installed fakekbd: '; md5sum /usr/local/bin/fakekbd | cut -d' ' -f1
    /usr/local/bin/rbkeyd list >/dev/null && echo 'rbkeyd runs ok'"

if [ "$SERVICE" = "1" ]; then
    echo "== systemd unit =="
    $S "cat > /etc/systemd/system/rbkeyd.service" < "$REPO/scripts/rb/device/rbkeyd.service"
    $S "systemctl daemon-reload 2>/dev/null && systemctl enable --now rbkeyd.service 2>&1 | tail -1; systemctl is-active rbkeyd.service 2>&1"
fi

echo
echo "done."
echo "  map          : ssh $HOST '/usr/local/bin/rbkeyd list'"
echo "  status/log   : ssh $HOST 'systemctl status rbkeyd; tail -5 /tmp/rbkeyd.log'"
echo "  test (no kbd): ssh $HOST '/usr/local/bin/fakekbd p c up enter f1'"
echo "  scripted     : ssh $HOST '/usr/local/bin/rbkeyd send play 1'"
