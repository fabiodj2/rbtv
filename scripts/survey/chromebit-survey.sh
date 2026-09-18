#!/bin/sh
# Platform survey for the ASUS Chromebit CS10 (google-veyron-mickey).
# Run on the device as root:  sh chromebit-survey.sh
# Safe/read-only. Used to fill in docs/05-chromebit-survey.md.

say() { printf '\n===== %s =====\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

say "identity"
uname -a
cat /etc/os-release 2>/dev/null
echo "hostname: $(hostname)"
echo "uptime: $(uptime)"

say "device-tree"
for f in model compatible serial-number; do
	if [ -e "/proc/device-tree/$f" ]; then
		printf '%s: ' "$f"; tr '\0' ' ' < "/proc/device-tree/$f"; echo
	fi
done

say "cpuinfo"
cat /proc/cpuinfo

say "memory"
head -5 /proc/meminfo
free -m 2>/dev/null

say "kernel cmdline"
cat /proc/cmdline

say "storage"
df -h 2>/dev/null
echo "--- lsblk ---"; lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT 2>/dev/null
echo "--- /proc/partitions ---"; cat /proc/partitions

say "filesystems mounted"
mount | grep -vE 'proc|sysfs|devtmpfs|tmpfs|devpts|cgroup|debugfs|tracefs|pstore|efivarfs|securityfs|bpf|configfs|fusectl|mqueue|hugetlbfs|ramfs|autofs'

say "kernel modules"
lsmod 2>/dev/null

say "graphics / framebuffer"
ls -l /dev/fb* /dev/dri/* 2>/dev/null
for fb in /sys/class/graphics/fb*; do
	[ -e "$fb" ] || continue
	echo "--- $fb ---"
	for attr in name virtual_size stride bits_per_pixel; do
		[ -e "$fb/$attr" ] && printf '%s = %s\n' "$attr" "$(cat "$fb/$attr")"
	done
done
echo "--- drm connectors ---"
for c in /sys/class/drm/card*/card*-*; do
	[ -e "$c/status" ] || continue
	printf '%s: status=%s enabled=%s\n' "$c" \
		"$(cat "$c/status" 2>/dev/null)" "$(cat "$c/enabled" 2>/dev/null)"
	[ -s "$c/modes" ] && echo "  modes: $(tr '\n' ' ' < "$c/modes")"
done
echo "--- edid ---"
for e in /sys/class/drm/card*/card*-*/edid; do
	[ -s "$e" ] && echo "$e: $(wc -c < "$e") bytes"
done
if have modetest; then echo "--- modetest ---"; modetest -M rockchip 2>&1 | head -50; fi
if have fbset; then echo "--- fbset ---"; fbset -fb /dev/fb0 2>&1; fi

say "audio (ALSA)"
cat /proc/asound/cards 2>/dev/null
echo "--- devices ---"; cat /proc/asound/devices 2>/dev/null
echo "--- pcm ---"; cat /proc/asound/pcm 2>/dev/null
have aplay && aplay -l 2>&1
have amixer && { echo "--- controls ---"; amixer scontrols 2>&1 | head -20; }

say "USB"
have lsusb && lsusb
echo "--- dwc2 platform devices ---"
for d in /sys/bus/platform/drivers/dwc2/*; do [ -L "$d" ] && basename "$d"; done
echo "--- usb devices ---"
for u in /sys/bus/usb/devices/*; do
	[ -e "$u/idVendor" ] || continue
	printf '%s: %s:%s %s %s\n' "$(basename "$u")" \
		"$(cat "$u/idVendor" 2>/dev/null)" "$(cat "$u/idProduct" 2>/dev/null)" \
		"$(cat "$u/manufacturer" 2>/dev/null)" "$(cat "$u/product" 2>/dev/null)"
done

say "input devices"
cat /proc/bus/input/devices 2>/dev/null
have evtest && echo "(evtest present)" || echo "(no evtest)"

say "network"
ip -br addr 2>/dev/null
echo "--- routes ---"; ip route 2>/dev/null
if have nmcli; then echo "--- nmcli device ---"; nmcli device status 2>&1; fi
if have iw; then echo "--- iw dev ---"; iw dev 2>&1; fi
if have rfkill; then echo "--- rfkill ---"; rfkill list 2>&1; fi

say "thermal / cpufreq"
for z in /sys/class/thermal/thermal_zone*; do
	[ -e "$z/temp" ] && printf '%s %s = %s\n' "$z" "$(cat "$z/type" 2>/dev/null)" "$(cat "$z/temp" 2>/dev/null)"
done
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies 2>/dev/null

say "firmware (relevant)"
ls /lib/firmware 2>/dev/null | head -30
for d in mrvl brcm brcmfmac rtlwifi; do
	[ -d "/lib/firmware/$d" ] && echo "dir $d: $(ls "/lib/firmware/$d" | wc -l) files"
done

say "device-tree nodes of interest"
for pat in usb hdmi i2s i2c codec sound; do
	echo "--- $pat ---"
	ls -d /proc/device-tree/*${pat}* /proc/device-tree/*/*${pat}* 2>/dev/null | head -10
done

say "relevant packages"
if have apk; then apk info -v 2>/dev/null | grep -iE 'directfb|alsa|mesa|(^|-)drm|libinput|evtest|ffmpeg|sdl|pulse' | head -40; fi

say "tools present"
for t in gcc make rustc cargo docker 7z xz cpio fbset modetest evtest aplay amixer speaker-test i2cdetect; do
	printf '%-13s %s\n' "$t" "$(command -v "$t" 2>/dev/null || echo -)"
done

say "dmesg (filtered)"
dmesg 2>/dev/null | grep -iE 'rockchip|drm|hdmi|usb|dwc2|snd|asoc|codec|mmc|thermal|firmware|mwifiex|brcm' | tail -80

say "end"
