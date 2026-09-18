#!/bin/sh
# usb-probe.sh — report the live USB / rekordbox-DB detection state of `rbp`.
#
# Reads rbp's mount-info tables and UI globals through /proc/<pid>/mem (root).
# Addresses are from the XDJ-RX3 v1.20 binary (same image as PrimeBox/rb2go):
#
#   media kind 2 ("USB 1"): detect flag 0x03256888, property block 0x0325688c
#   media kind 3 ("USB 2"): detect flag 0x03256944, property block 0x03256948
#   uiConnectedMedia      0x326f8b4   bit1 = USB1
#   uiBrowse (browse mode) 0x326f8b8
#   Ui_BrowseDispDataUpdate 0x326e128
#
# detect flag: 0 = absent, 1 = analysing, 2 = ready (UI shows the drive).
# property block: +0 UTF-16LE label, +120 u32 songs, +126 u8 db-ready,
#                 +128 u32 playlists, +132/+136 capacity hi/lo,
#                 +140/+144 free hi/lo.
#
# Usage:  sh /home/user/usb-probe.sh [-q]      (-q: no section headers)
# Exit:   0 if USB1 is detected (flag==2), 1 otherwise.

CH=/home/user/rbx3-run
MNT=/media/usb1/sda1
CH_MNT=$CH/media/usb1/sda1

rbp_pid() { pgrep -f '/root/pdj/rbp -a' 2>/dev/null | head -1; }

P=$(rbp_pid)
if [ -z "$P" ]; then
    echo "rbp:         NOT RUNNING"
    exit 1
fi

echo "rbp pid:     $P"

# host-side mount / label / db presence
if mountpoint -q "$MNT" 2>/dev/null; then
    dev=$(awk -v m="$MNT" '$2==m {print $1}' /proc/mounts | head -1)
    echo "mount:       $MNT <- $dev ($(blkid -s TYPE -o value "$dev" 2>/dev/null))"
    echo "label:       $(blkid -s LABEL -o value "$dev" 2>/dev/null)"
    echo "export.pdb:  $(ls -l "$MNT/PIONEER/rekordbox/export.pdb" 2>/dev/null | awk '{print $5" bytes"}')"
else
    echo "mount:       $MNT NOT MOUNTED"
fi
echo "chroot bind: $(mountpoint -q "$CH_MNT" && echo yes || echo no)"

python3 - "$P" <<'PY'
import struct, sys
pid = int(sys.argv[1])
def rd(addr, n):
    with open('/proc/%d/mem' % pid, 'rb') as f:
        f.seek(addr); return f.read(n)
def u32(addr): return struct.unpack('<I', rd(addr, 4))[0]

KIND = [(2, 0x03256888, 0x0325688c, "USB1"),
        (3, 0x03256944, 0x03256948, "USB2")]

ok = False
for kind, det_a, prop_a, name in KIND:
    det = u32(det_a)
    prop = rd(prop_a, 168)
    label = prop[0:64].decode('utf-16-le', 'ignore').split('\x00')[0]
    songs = struct.unpack('<I', prop[120:124])[0]
    dbrdy = prop[126]
    pls   = struct.unpack('<I', prop[128:132])[0]
    cap_h, cap_l = struct.unpack('<II', prop[132:140])
    free_h, free_l = struct.unpack('<II', prop[140:148])
    cap  = ((cap_h << 32) | cap_l) / 1e9
    free = ((free_h << 32) | free_l) / 1e9
    state = {0: "absent", 1: "analysing", 2: "READY"}.get(det, "?")
    print("kind %d %-4s: detect=%d (%s)  label=%-14s songs=%-6d playlists=%-5d "
          "db_ready=%d  cap=%.1fGB free=%.1fGB"
          % (kind, name, det, state, repr(label), songs, pls, dbrdy, cap, free))
    if kind == 2 and det == 2:
        ok = True

cm = u32(0x326f8b4)
bm = u32(0x326f8b8)
print("uiConnectedMedia = 0x%x (bit1 USB1: %s)   uiBrowse = %d"
      % (cm, "set" if cm & 2 else "clear", bm))
sys.exit(0 if ok else 1)
PY
exit $?
