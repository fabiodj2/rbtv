#!/bin/sh
# quiet-console.sh - keep the rekordbox player (DirectFB) in the foreground by
# stopping everything that paints text onto the HDMI framebuffer console.
#
# Why: the Chromebit boots with `console=tty0 loglevel=8`, so every kernel
# message (USB enumeration retries, FAT warnings, ...) is drawn straight onto
# /dev/fb0 - i.e. text flickering over the player.  On top of that
# getty@tty1 was enabled, so a login prompt also lived on that screen.
#
# What this sets (idempotent, survives reboot):
#   1. /etc/sysctl.d/99-rb-console.conf  kernel.printk = 1 4 1 7
#      -> only KERN_EMERG reaches the console
#   2. /etc/systemd/system.conf.d/99-rb-quiet.conf  ShowStatus=no
#      -> systemd's "[ OK ] Started ..." lines stay out of the console
#   3. getty@tty1 disabled + stopped
#      -> no login prompt over the player (SSH keeps working)
#
# start-rb.sh additionally detaches fbcon from the framebuffer before it
# launches rbp (QUIET_CONSOLE=2, the default) - that is the last line of
# defence: nothing can overdraw DirectFB at all.
#
# Usage (on the Chromebit, as root):
#   sh /home/user/quiet-console.sh            # apply
#   sh /home/user/quiet-console.sh status     # show current state
#   sh /home/user/quiet-console.sh revert     # get the console back
set -u

SYSCTL=/etc/sysctl.d/99-rb-console.conf
SYSDROP=/etc/systemd/system.conf.d/99-rb-quiet.conf

apply() {
    echo "== 1. kernel printk (console gets KERN_EMERG only) =="
    mkdir -p /etc/sysctl.d
    cat > "$SYSCTL" <<'EOF'
# chromebit: keep rbp (DirectFB) in the foreground on the HDMI framebuffer.
# Only KERN_EMERG is printed to the console; use `dmesg`/journal for the rest.
# See docs/07-display-working.md.
kernel.printk = 1 4 1 7
EOF
    sysctl -p "$SYSCTL" >/dev/null 2>&1 || true
    echo "   /proc/sys/kernel/printk = $(cat /proc/sys/kernel/printk)"

    echo "== 2. systemd status output off =="
    mkdir -p /etc/systemd/system.conf.d
    cat > "$SYSDROP" <<'EOF'
# chromebit: do not print unit start/stop status onto the framebuffer console
[Manager]
ShowStatus=no
EOF
    systemctl daemon-reexec 2>/dev/null || true

    echo "== 3. console login prompt =="
    systemctl disable --now getty@tty1 2>&1 | sed 's/^/   /' || true
    echo "   getty@tty1: enabled=$(systemctl is-enabled getty@tty1 2>&1) active=$(systemctl is-active getty@tty1 2>&1)"
    echo "   (serial/tty getty for other consoles is left alone; SSH is unaffected)"

    echo "== 4. fbcon =="
    echo "   vtcon1 bind=$(cat /sys/class/vtconsole/vtcon1/bind 2>/dev/null) " \
         "(start-rb.sh detaches it when it launches rbp)"
    echo "done. restart the player to repaint a clean screen:  sh /home/user/start-rb.sh"
}

status() {
    echo "printk:          $(cat /proc/sys/kernel/printk)"
    echo "sysctl file:     $([ -f "$SYSCTL" ] && echo present || echo missing)"
    echo "systemd drop-in: $([ -f "$SYSDROP" ] && echo present || echo missing)"
    echo "getty@tty1:      enabled=$(systemctl is-enabled getty@tty1 2>&1) active=$(systemctl is-active getty@tty1 2>&1)"
    echo "fbcon vtcon1:    bind=$(cat /sys/class/vtconsole/vtcon1/bind 2>/dev/null)"
    echo "cursor blink:    $(cat /sys/class/graphics/fbcon/cursor_blink 2>/dev/null)"
    echo "fgconsole:       $(fgconsole 2>/dev/null || echo '?')"
}

revert() {
    echo "== restoring the framebuffer console =="
    rm -f "$SYSCTL" "$SYSDROP"
    sysctl -w kernel.printk="8 4 1 7" >/dev/null 2>&1 || true
    systemctl daemon-reexec 2>/dev/null || true
    systemctl enable --now getty@tty1 2>&1 | sed 's/^/   /' || true
    [ -w /sys/class/vtconsole/vtcon1/bind ] && echo 1 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null
    echo "   printk=$(cat /proc/sys/kernel/printk) fbcon=$(cat /sys/class/vtconsole/vtcon1/bind 2>/dev/null)"
}

case "${1:-apply}" in
    apply|start) apply ;;
    status)      status ;;
    revert|stop) revert ;;
    *) echo "usage: $0 [apply|status|revert]" >&2; exit 2 ;;
esac
