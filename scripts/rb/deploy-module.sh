#!/bin/sh
# deploy-module.sh — fast iteration: push the freshly built fbdev module (and
# optionally the patched rbp) to a running Chromebit and install them into the
# chroot, keeping a .prev backup.
#
# Usage:
#   PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-module.sh
#   RBP=1 PASS=<password> ./scripts/rb/deploy-module.sh      # also install rbp
#
# Then (re)start the player:
#   ssh root@chromebit 'sh /home/user/start-rb.sh'
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=${HOST:-root@chromebit.local}
PASS=${PASS:?set PASS to the device password}
MODULE=${MODULE:-$REPO/work/rb/dfb/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so}
RBPBIN=${RBPBIN:-$REPO/work/rb/rbp-chromebit}
CH=/home/user/rbx3-run
S="sshpass -p $PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $HOST"

[ -f "$MODULE" ] || { echo "module not found: $MODULE" >&2; exit 1; }

echo "== upload fbdev module ($(basename "$MODULE")) =="
$S "cat > /home/user/libdirectfb_fbdev.new" < "$MODULE"
$S "set -e
    cp -f $CH/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so /home/user/libdirectfb_fbdev.prev 2>/dev/null || true
    cp -f /home/user/libdirectfb_fbdev.new $CH/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so
    echo -n 'installed driver: '; md5sum $CH/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so | cut -d' ' -f1"

echo "== upload shims (memshim/audioshim/keyshim) =="
for shim in memshim audioshim keyshim; do
    SO=$REPO/work/rb/shims/$shim.so
    [ -f "$SO" ] || { echo "  (skip $shim.so — build with scripts/rb/build-shims.sh)"; continue; }
    $S "cat > /home/user/$shim.new" < "$SO"
    $S "cp -f $CH/usr/lib/$shim.so /home/user/$shim.prev 2>/dev/null || true
        cp -f /home/user/$shim.new $CH/usr/lib/$shim.so
        echo -n '  installed $shim.so: '; md5sum $CH/usr/lib/$shim.so | cut -d' ' -f1"
done

echo "== upload display/audio diagnostics =="
$S "cat > /home/user/check-display.sh" < "$REPO/scripts/rb/device/check-display.sh"
$S "cat > /home/user/check-audio.sh"   < "$REPO/scripts/rb/device/check-audio.sh"
$S "cat > /home/user/press-key.sh"     < "$REPO/scripts/rb/device/press-key.sh"
$S "cat > /home/user/usb-watch.sh"     < "$REPO/scripts/rb/device/usb-watch.sh"
$S "cat > /home/user/usb-probe.sh"     < "$REPO/scripts/rb/device/usb-probe.sh"
$S "chmod 755 /home/user/check-display.sh /home/user/check-audio.sh /home/user/press-key.sh /home/user/usb-watch.sh /home/user/usb-probe.sh"

echo "== upload launcher (LD_PRELOAD order: memshim:fbshim:audioshim) =="
$S "cat > /home/user/start-rb.sh" < "$REPO/scripts/rb/start-rb.sh"
$S "chmod 755 /home/user/start-rb.sh"

# Keep the player in the foreground: silence the framebuffer console
# (kernel printk + getty login prompt). Idempotent; see docs/07.
if [ "${QUIET_CONSOLE:-1}" = "1" ]; then
    echo "== quiet the framebuffer console (printk + getty) =="
    $S "cat > /home/user/quiet-console.sh" < "$REPO/scripts/rb/device/quiet-console.sh"
    $S "chmod 755 /home/user/quiet-console.sh; sh /home/user/quiet-console.sh apply | tail -6"
fi

if [ "${RBP:-0}" = "1" ]; then
    [ -f "$RBPBIN" ] || { echo "rbp not found: $RBPBIN" >&2; exit 1; }
    echo "== upload rbp =="
    $S "cat > /home/user/rbp-chromebit" < "$RBPBIN"
    $S "set -e
        chmod 755 /home/user/rbp-chromebit
        cp -f $CH/root/pdj/rbp $CH/root/pdj/rbp.prev 2>/dev/null || true
        cp -f /home/user/rbp-chromebit $CH/root/pdj/rbp
        chmod 755 $CH/root/pdj/rbp
        echo -n 'installed rbp:    '; md5sum $CH/root/pdj/rbp | cut -d' ' -f1"
fi

echo
echo "done. restart:  ssh $HOST 'sh /home/user/start-rb.sh'"
echo "rollback:       ssh $HOST 'cp /home/user/libdirectfb_fbdev.prev $CH/usr/lib/directfb-1.4-6/systems/libdirectfb_fbdev.so'"
