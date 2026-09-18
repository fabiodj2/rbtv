#!/bin/sh
# deploy-chromebit.sh — ship the assembled RX3 chroot to a Chromebit and install
# the device glue (fix-dev.sh, start-rb.sh).
#
# Prerequisite: the tarball (default /tmp/rx3-run.tar.gz) produced by staging
# the chroot (see docs/06). It must already contain the corrected shims:
#   usr/lib/fbshim.so    <- rebuilt fbshim16-phone.c (r7=54, NOT fork)
#   usr/lib/memshim.so   <- denies /dev/mem + redirects the broken MAP_SHARED mmap
#
# Usage:
#   PASS=<password> HOST=root@chromebit.local ./scripts/rb/deploy-chromebit.sh
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=${HOST:-root@chromebit.local}
PASS=${PASS:?set PASS to the device password}
TARBALL=${TARBALL:-/tmp/rx3-run.tar.gz}
[ -f "$TARBALL" ] || { echo "tarball missing: $TARBALL"; exit 1; }

S="sshpass -p $PASS ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $HOST"

echo "== upload tarball ($(du -h "$TARBALL" | cut -f1)) =="
$S 'mkdir -p /home/user/incoming'
$S 'cat > /home/user/incoming/rx3-run.tar.gz' < "$TARBALL"

echo "== upload device glue =="
$S 'cat > /home/user/fix-dev.sh' < "$REPO/scripts/rb/fix-dev.sh"
$S 'cat > /home/user/start-rb.sh' < "$REPO/scripts/rb/start-rb.sh"
$S 'cat > /home/user/usb-watch.sh' < "$REPO/scripts/rb/device/usb-watch.sh"
$S 'cat > /home/user/usb-probe.sh' < "$REPO/scripts/rb/device/usb-probe.sh"

# optional boot-time watcher unit (enable manually once rb is a service)
$S 'cat > /etc/systemd/system/rb-usbwatch.service' < "$REPO/scripts/rb/device/rb-usbwatch.service"

# ship the device-side diagnostics (display/audio/key/usb)
for t in check-display check-audio press-key; do
    $S "cat > /home/user/$t.sh" < "$REPO/scripts/rb/device/$t.sh"
done

echo "== extract on device =="
$S 'set -e
    chmod 755 /home/user/fix-dev.sh /home/user/start-rb.sh /home/user/usb-watch.sh /home/user/usb-probe.sh
    chmod 755 /home/user/check-display.sh /home/user/check-audio.sh /home/user/press-key.sh
    cd /home/user
    for m in dev proc sys tmp; do umount /home/user/rbx3-run/$m 2>/dev/null || true; done
    rm -rf /home/user/rbx3-run
    tar xzf incoming/rx3-run.tar.gz
    echo "loader: $(readlink rbx3-run/lib/ld-linux.so.3)"
    md5sum rbx3-run/root/pdj/rbp rbx3-run/usr/lib/fbshim.so
    du -sh rbx3-run'

echo
echo "deployed to $HOST:/home/user/rbx3-run"
echo "next:  ssh $HOST 'sh /home/user/start-rb.sh'   (or run /root/dfbtest first)"
