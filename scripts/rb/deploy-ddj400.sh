#!/bin/sh
# deploy-ddj400.sh — install the DDJ-400 -> rbp control path on a Chromebit.
#
#   * ddj400-bridge    built natively on the device (no dependencies, uses the
#                      ALSA rawmidi device directly) -> /usr/local/bin
#   * keyshim.so       rebuilt with /tmp/rb-ctrl.fifo support -> chroot usr/lib
#   * start-rb.sh      creates both FIFOs and starts the bridge
#   * ddj400-start.sh  manual start/stop/status/sniff helper
#   * optional systemd unit (INSTALL_SERVICE=1, default 1 when systemd is present)
#
# Usage:
#   PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-ddj400.sh
#   SKIP_SHIM=1 ...            # only rebuild/install the bridge
#   SERVICE=0   ...            # do not install the systemd unit
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=${HOST:-root@chromebit.local}
PASS=${PASS:?set PASS to the device password}
CH=${CH:-/home/user/rbx3-run}
SERVICE=${SERVICE:-1}
S="sshpass -p $PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $HOST"
SRC=$REPO/scripts/rb/device/ddj400-bridge.c
SHIM=$REPO/work/rb/shims/keyshim.so

[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }

echo "== upload bridge source and build it on the device =="
$S "cat > /tmp/ddj400-bridge.c" < "$SRC"
$S "set -e
    command -v gcc >/dev/null || { echo 'no gcc on the device' >&2; exit 1; }
    gcc -O2 -Wall -Wextra -o /tmp/ddj400-bridge /tmp/ddj400-bridge.c
    install -m 755 /tmp/ddj400-bridge /usr/local/bin/ddj400-bridge
    echo -n 'installed bridge: '; md5sum /usr/local/bin/ddj400-bridge | cut -d' ' -f1
    /usr/local/bin/ddj400-bridge -l | head -1 >/dev/null && echo 'bridge runs ok'"

if [ "${SKIP_SHIM:-0}" != "1" ]; then
    if [ -f "$SHIM" ]; then
        echo "== install keyshim.so (ctrl FIFO support) into the chroot =="
        $S "cat > /home/user/keyshim.new" < "$SHIM"
        $S "set -e
            cp -f $CH/usr/lib/keyshim.so /home/user/keyshim.prev 2>/dev/null || true
            cp -f /home/user/keyshim.new $CH/usr/lib/keyshim.so
            echo -n 'installed keyshim.so: '; md5sum $CH/usr/lib/keyshim.so | cut -d' ' -f1"
    else
        echo "!! $SHIM not built — run scripts/rb/build-shims.sh keyshim.so" >&2
    fi
fi

echo "== upload launcher + helper =="
$S "cat > /home/user/start-rb.sh"     < "$REPO/scripts/rb/start-rb.sh"
$S "cat > /home/user/ddj400-start.sh" < "$REPO/scripts/rb/device/ddj400-start.sh"
$S "cat > /home/user/ddj400-selftest.sh" < "$REPO/scripts/rb/device/ddj400-selftest.sh"
$S "cat > /home/user/quiet-console.sh" < "$REPO/scripts/rb/device/quiet-console.sh"
$S "chmod 755 /home/user/start-rb.sh /home/user/ddj400-start.sh /home/user/ddj400-selftest.sh /home/user/quiet-console.sh"

# Keep the player in the foreground on the HDMI console (docs/07).
if [ "${QUIET_CONSOLE:-1}" = "1" ]; then
    echo "== quiet the framebuffer console (printk + getty + systemd status) =="
    $S "sh /home/user/quiet-console.sh apply 2>&1 | tail -7"
fi

if [ "$SERVICE" = "1" ]; then
    echo "== install systemd unit (optional; bridge is also started by start-rb.sh) =="
    $S "cat > /etc/systemd/system/ddj400-bridge.service" < "$REPO/scripts/rb/device/ddj400-bridge.service"
    $S "systemctl daemon-reload 2>/dev/null && systemctl enable ddj400-bridge.service 2>/dev/null && echo 'unit enabled (starts on boot)' || echo 'systemd not available — start-rb.sh handles it'"
fi

echo
echo "done."
echo "  start player + bridge : ssh $HOST 'sh /home/user/start-rb.sh'"
echo "  bridge status         : ssh $HOST 'sh /home/user/ddj400-start.sh status'"
echo "  raw MIDI sniff        : ssh $HOST 'sh /home/user/ddj400-start.sh sniff'"
