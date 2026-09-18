#!/bin/sh
# check-audio.sh — run ON the Chromebit (root) to diagnose the audio path while
# rbp is running.  Shows the HDMI PCM parameters the shim negotiated, whether the
# stream is actually running, and the audioshim write counter.
#
# Usage:  sh check-audio.sh
echo "== ALSA cards =="
cat /proc/asound/cards

echo
echo "== playback devices =="
aplay -l 2>/dev/null | head -12

echo
echo "== HDMI PCM hw params (rbp owns it while running) =="
cat /proc/asound/card0/pcm0p/sub0/hw_params 2>&1

echo
echo "== HDMI PCM status =="
cat /proc/asound/card0/pcm0p/sub0/status 2>&1

echo
echo "== HDMI routing =="
amixer -c 0 cget numid=1 2>/dev/null | grep -E 'name=|values='
amixer -c 0 cget numid=6 2>/dev/null | grep -E 'name=|values='

echo
echo "== audioshim negotiation =="
grep -E "opened real|real set_|real hw_params" /tmp/audioshim.log 2>/dev/null | tail -10

echo
echo "== audioshim write counter (should keep increasing) =="
grep -c "writei #" /tmp/audioshim.log 2>/dev/null
echo "last lines:"
grep "writei #" /tmp/audioshim.log 2>/dev/null | tail -3

echo
echo "== rbp =="
ps -eo pid,%cpu,args 2>/dev/null | grep "[r]bp -a"
