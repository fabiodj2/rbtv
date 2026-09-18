#!/bin/sh
# start-rb.sh — launch the XDJ-RX3 player from the Chromebit chroot.
# Deployed to /home/user/start-rb.sh. Run as root.
#
# Chromebit-specific vs the Prime GO original:
#   * DFB_ROTATE=off   (landscape HDMI; scale/convert only, no rotation)
#   * no knobshim      (no usable USB gadget / knob hardware yet)
#   * audioshim maps the RX3 CS4344 devices onto the HDMI PCM hw:0,0 (stereo)
#   * keyshim takes button events from /tmp/rb-keys.fifo and controller
#     values (faders/EQ/jog) from /tmp/rb-ctrl.fifo  -> docs/09, docs/12
#   * the host-side ddj400-bridge feeds rb-ctrl.fifo from a real DDJ-400
CH=/home/user/rbx3-run
# order matters: memshim first (open/read/mmap/poll fixups), then fbshim (fb ioctl),
# audioshim (snd_* -> HDMI hw:0,0), keyshim (FIFO -> sendKey + UiMain pump).
# See docs/07 (display), docs/08 (audio), docs/09 (input).
PRELOAD=${PRELOAD:-/usr/lib/memshim.so:/usr/lib/fbshim.so:/usr/lib/audioshim.so:/usr/lib/keyshim.so}
LOGDIR=/home/user/rbx3-run/tmp

# 1. device binds + RX3 stubs
sh /home/user/fix-dev.sh || exit 1

# 1b. idempotent: never leave a second player behind (this script is also run
#     by rb.service on every boot)
pkill -f '[r]bp -a' 2>/dev/null && { echo "stopped previous player"; sleep 2; }
pkill -f '[e]db_streamd' 2>/dev/null || true

# 2. stale IPC / logs
rm -f /tmp/guard_LocalDBServer /tmp/req_LocalDBServer
rm -f /tmp/audioshim.log /tmp/keyshim.log /tmp/knobshim.log /tmp/dfbdig*.log
# The FIFOs are recreated to drop any stale records, but that orphans the fds
# held by the writer daemons (rbkeyd, ddj400-bridge): they keep writing into a
# deleted inode and the keyboard/controller goes dead until they are restarted.
# Reopen them after the FIFOs exist again.  See docs/13 "restart gotcha".
rm -f /tmp/rb-keys.fifo /tmp/rb-ctrl.fifo 2>/dev/null
[ -p /tmp/rb-keys.fifo ] || mkfifo /tmp/rb-keys.fifo 2>/dev/null
[ -p /tmp/rb-ctrl.fifo ] || mkfifo /tmp/rb-ctrl.fifo 2>/dev/null
chmod 666 /tmp/rb-keys.fifo /tmp/rb-ctrl.fifo 2>/dev/null
systemctl try-restart rbkeyd.service 2>/dev/null || true
if [ -x /home/user/ddj400-start.sh ]; then
    sh /home/user/ddj400-start.sh restart >/dev/null 2>&1 || true
fi

# 2b. Keep rbp in the foreground: once the player starts, silence the
#     framebuffer console (kernel messages, login prompt) and detach fbcon so
#     nothing can overdraw DirectFB.  See docs/07 and quiet-console.sh.
#       QUIET_CONSOLE=0  keep the console (debugging)
#       QUIET_CONSOLE=1  silence kernel messages only
#       QUIET_CONSOLE=2  (default) also detach fbcon from the framebuffer
#     Re-attach later with:  echo 1 > /sys/class/vtconsole/vtcon1/bind
QC=${QUIET_CONSOLE:-2}
if [ "$QC" != 0 ]; then
    dmesg -n 1 2>/dev/null || true
    [ -w /sys/class/graphics/fbcon/cursor_blink ] && \
        echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null
    if [ "$QC" = 2 ] && [ -w /sys/class/vtconsole/vtcon1/bind ]; then
        echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null && \
            echo "fbcon detached (QUIET_CONSOLE=2, rebind: echo 1 > /sys/class/vtconsole/vtcon1/bind)"
    fi
fi

# 3. DeviceSQL daemon (rbp looks for its IPC sockets)
nohup chroot "$CH" /lib/ld-linux.so.3 /usr/bin/edb_streamd \
    > "$LOGDIR/edb_d.log" 2>&1 &
sleep 1

# 4. player
# Safety net during bring-up: cap processes/threads so a runaway init cannot
# fork-bomb and OOM the Chromebit (this is what corrupted the rootfs before).
# rbp normally runs ~40 threads; 1024 is generous.
ulimit -u 1024 2>/dev/null || true
nohup chroot "$CH" env DFB_ROTATE=off STARTUP_MUTE_MS=1500 STARTUP_FADE_MS=300 LD_PRELOAD="$PRELOAD" \
    /lib/ld-linux.so.3 /root/pdj/rbp -a \
    </dev/null > "$LOGDIR/rbp.log" 2>&1 &

# 5. USB stick watcher. Mounts any rekordbox stick on the dwc2 host port at
#    /media/usb1/sda1, binds it into the chroot and writes the mount event to
#    /tmp/udev_usb1. rbp then runs its native DeviceSQL import of export.pdb
#    (multi-track library, not just FOLDER view). Idempotent: keeps running and
#    re-notifies when rbp restarts. See docs/11-usb.md.
mkdir -p "$CH/media/usb1/sda1"
if [ -x /home/user/usb-watch.sh ]; then
    sh /home/user/usb-watch.sh start
fi

echo "launched rbp; log: $LOGDIR/rbp.log"
echo "usb stick hotplug watcher: /home/user/usbwatch.log"
echo "keyboard (rbkeyd) and DDJ-400 bridge run as their own systemd units"

# 6. DDJ-400 MIDI bridge (host rootfs, feeds /tmp/rb-ctrl.fifo). Only if
#    installed; it waits for the controller to be plugged in. See docs/12.
if [ -x /home/user/ddj400-start.sh ]; then
    sh /home/user/ddj400-start.sh start || true
fi
