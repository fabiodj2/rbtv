# 13 — Physical keyboard control (rb2go-style)

Operate the rekordbox engine with a **USB keyboard** plugged into the Chromebit,
the way rb2go's `rbviewer` did on the POCO X3 — this is the fallback when the
DDJ-400 is not connected (the box has a single USB-A port, so it is normally one
or the other, or both behind a powered hub).

rb2go used an SDL2 window because that platform had Wayland. The Chromebit has
**no X/Wayland** (rb draws straight to the framebuffer through DirectFB), so the
port is headless: an **evdev** reader that feeds the same FIFOs `keyshim.so`
already consumes.

```
 USB keyboard ──evdev──▶ rbkeyd ──▶ /tmp/rb-keys.fifo (buttons, 12 B)
                                    /tmp/rb-ctrl.fifo (values,  24 B)
                                              │
                                              ▼
                        keyshim.so ──▶ IKeyManager::sendKey(key, op, ch, param, f, l)
```

Status: **working, verified on real hardware** — buttons, browse/list+menu
navigation, jog, pads, mixer values and tempo all reach the engine.  Verified
with a Logitech K400 Plus wireless keyboard (`rbkeyd: keyboard /dev/input/event4
(Logitech K400 Plus)`); presses show up as `sendkey key=00004101` etc. in
`/tmp/keyshim.log` and the UI reacts.

The daemon runs as `rbkeyd.service` (enabled at boot) and `rb.service` starts the
player, so a reboot needs no SSH at all.

## 1. Components

| File | Role |
|---|---|
| `scripts/rb/device/rbkeyd.c` | the daemon: evdev → FIFOs, key map, hot-plug |
| `scripts/rb/deploy-keyboard.sh` | build on the device, install, enable `rbkeyd.service` |
| `scripts/rb/device/fakekbd.c` | uinput test keyboard (inject keys with no hardware) |

`rbkeyd` runs on the **host rootfs** (like the DDJ-400 bridge): it only needs
`/dev/input/event*` and the shared `/tmp`, has no dependencies, and is compiled
by the device's own gcc in a second.

## 2. Key map

Active deck = 1; **TAB** switches it (the log prints `active deck N`).

| Keys | Action | rbp keycode |
|---|---|---|
| `m` `s` `b` `u` `r` `l` `i` `t` | menu, source, browse, usb1, rekordbox, link, info, tag list | 0x0206, 0x0201, 0x0202, 0x0209, 0x0208, 0x0207, 0x020b, 0x0203 |
| `ENTER` | select (browse knob push) | 0x420c |
| `BACKSPACE` | back | 0x420d |
| `UP` / `DOWN` | browse knob: move selection up / down in the track list **and in menus** | 0x420c op4 (−1 / +1) |
| `PGUP` / `PGDN` | same, 10 steps | 0x420c op4 |
| `1` / `2` | load deck 1 / 2 | 0x4311 ch1/ch2 |
| `p` / `o` | play/pause deck 1 / 2 | 0x4101 |
| `c` / `v` | cue deck 1 / 2 | 0x4102 |
| `y` / `h` | sync deck 1 / 2 | 0x4112 |
| `k` | master (active deck) | 0x4111 |
| `g` | tempo range (active deck) | 0x4107 |
| `[` / `]` | loop in / loop out | 0x410c / 0x410d |
| `\` | reloop / exit | 0x410e |
| `F1`…`F8` | pads 1…8 (active deck) | 0x4117…0x411e |
| `F9` `F10` `F11` `F12` | pad bank: hot cue / auto loop / slip loop / beat jump | 0x4113 / 0x4114 / 0x4115 / 0x4116 |
| `LEFT` / `RIGHT` | jog active deck (nudge / bend) | 0x4305 op4 |
| `SHIFT`+`LEFT`/`RIGHT` | jog, coarse | 0x4305 op4 |

`m s b u r l i t / p o c v y h / 1 2` are rb2go's original letters, unchanged.

### Direction of the arrows

rbp has **no dedicated menu-up/down keycodes** — the XDJ-RX3 navigates both the
track list and its menus with the rotary selector (`K_SELECTOR 0x420c`, op 4,
±1).  The arrows drive that knob, so their direction is what decides whether the
highlight moves the way you expect:

| selector turn | engine value | list / menu effect |
|---|---|---|
| clockwise | `+1` | highlight moves **down** |
| counter-clockwise | `−1` | highlight moves **up** |

Therefore `UP` sends `−1` and `DOWN` sends `+1` (and `PGUP`/`PGDN` the same, in
steps of 10).  If the arrows ever feel inverted in the UI, that sign is the only
thing to change — one line in `handle_key()`, then re-run `deploy-keyboard.sh`.

### Mixer layer — press `INSERT` to toggle

While the layer is on, the letters drive the mixer strip of the **active deck**
(continuous values, all through the control FIFO). Press `INSERT` again to go
back to buttons.

| Keys | Control | keycode / op |
|---|---|---|
| `w` / `s` | channel fader up / down | 0x501e op4 |
| `e` / `d` | TRIM up / down | 0x5019 op4 |
| `r` / `f` | EQ HI up / down | 0x501a op4 |
| `t` / `g` | EQ MID up / down | 0x501b op4 |
| `y` / `h` | EQ LOW up / down | 0x501c op4 |
| `u` / `j` | Sound Color FX (filter) up / down | 0x509d op5 |
| `o` / `l` | CROSSFADER right / left (global) | 0x6017 op4 |
| `-` / `=` | TEMPO slider down / up | 0x4109 op5 (±0.02 per press) |

Values ramp from the engine's own defaults (fader up = 1023, TRIM/EQ/colour and
crossfader centred = 512, tempo 0), so the keyboard never fights what `keyshim`
pushed at startup. Note the fader already starts at maximum — use `s` to bring a
channel down. Pressing any mixer-layer key also stops `keyshim` re-applying the
synthetic mixer defaults (same rule as the DDJ-400, docs/12 §2).

## 3. One-shot CLI (scripting, and everything not on a key)

`rbkeyd send` injects a single control — useful over SSH, in scripts, or for the
mixer controls you do not want on a key:

```sh
rbkeyd send list                       # all control names
rbkeyd send play 1                     # button on deck 1
rbkeyd send loopin 2
rbkeyd send pad 3 2                    # pad 3 on deck 2
rbkeyd send hotcue 1                   # pad bank
rbkeyd send jog 1 8                    # jog deck 1 by +8
rbkeyd send select                     # global button
rbkeyd send fader 1 900                # 10-bit value (op4)
rbkeyd send eqlow 2 700
rbkeyd send color 1 512                # colour FX (op5)
```

## 4. Build & deploy

```sh
WORKSTATION$ PASS=<password> HOST=root@chromebit.local \
             ./scripts/rb/deploy-keyboard.sh
```

That compiles `rbkeyd`/`fakekbd` on the Chromebit, installs them to
`/usr/local/bin`, and enables:

```
/etc/systemd/system/rbkeyd.service      Restart=always, logs to /tmp/rbkeyd.log
```

`rbkeyd.service` is hot-plug aware: it waits for a keyboard, re-scans when one
is unplugged, and prefers a physical keyboard over a remapping daemon's virtual
device — so a wireless keyboard dongle plugged in after boot just works.
`SERVICE=0` skips the unit if you prefer to run it by hand.

Device selection: rbkeyd rescans every 2 s and drives the **best free** device —
a real, ungrabbed keyboard (it even switches away from a virtual one when a
physical keyboard appears, and falls back when it is unplugged), e.g.:

```
rbkeyd: keyboard /dev/input/event2 (keyd virtual keyboard)
rbkeyd: switching keyboard /dev/input/event2 -> /dev/input/event4
rbkeyd: keyboard /dev/input/event4 (Logitech K400 Plus)
rbkeyd: keyboard /dev/input/event4 gone, rescanning ...
```

The grab test matters — on this image `keyd` runs and creates a *keyd virtual
keyboard*; if keyd ever grabs the physical device, rbkeyd skips it and uses the
virtual one instead. Choose explicitly with:

```sh
rbkeyd -n "SEMICO"          # match the device name
rbkeyd -d /dev/input/event7 # exact node
```

rbkeyd does **not** grab the keyboard itself, so it cannot break other
consumers. It reads raw keys from the physical device (keyd remapping, if any,
is applied on keyd's virtual device and is therefore not seen by rbkeyd).

## 5. Run

```sh
CHROMEBIT# systemctl status rbkeyd; tail -f /tmp/rbkeyd.log
CHROMEBIT# systemctl restart rbkeyd
CHROMEBIT# /usr/local/bin/rbkeyd list          # print the map
CHROMEBIT# /usr/local/bin/rbkeyd -v -n SEMICO  # foreground, verbose
```

## 6. Verify

Without a keyboard (or to automate the check), `fakekbd` creates a virtual
keyboard through `/dev/uinput` and injects keys. It also exercises hot-plug,
because the device appears and disappears:

```sh
CHROMEBIT# sh /home/user/ddj400-selftest.sh           # MIDI chain (docs/12 §6)
CHROMEBIT# /usr/local/bin/fakekbd p c y 1 b           # press those keys
CHROMEBIT# tail -5 /tmp/keyshim.log                   # events reached the engine
```

A full pass (buttons, browse, pads, mixer layer) was verified this way:

```
rbkeyd: keyboard /dev/input/event9 (rbkeyd-test-keyboard)
  sync deck 1 -> 0x4112 ch1
  browse +1                          ctrl key=0x420c op=4 ch=1 param=1
  select (browse push) -> 0x420c ch1
rbkeyd: active deck 2
  pad bank hot cue -> 0x4113 ch2     pad 2 -> 0x4118 ch2
rbkeyd: MIXER layer ON
  key=0x5019 ch2 val=528             ctrl key=0x5019 op=4 ch=2 param=528
  tempo deck 2 norm=-0.02            ctrl key=0x4109 op=5 ch=2 param=501
```

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `waiting for a keyboard on /dev/input ...` | no keyboard, or it is not `EV_KEY`-capable in the expected way | plug it; check `cat /proc/bus/input/devices` |
| `keyboard device(s) are grabbed by another program (keyd?)` | another daemon holds it exclusively | use its virtual device, or `rbkeyd -n <name>` / `-d <node>` |
| keys do nothing, but the log shows the actions | rbp/keyshim not running, or FIFOs missing | `sh /home/user/start-rb.sh`, then `ls -l /tmp/rb-*.fifo` |
| keys go dead after `systemctl restart rb.service` | the restart recreated `/tmp/rb-*.fifo`; rbkeyd/ddj400-bridge still hold the deleted FIFO and write into nowhere | `systemctl restart rbkeyd` + `sh /home/user/ddj400-start.sh restart` (current `start-rb.sh` does this itself — docs/15) |
| only the deck letters work, not `w`/`e`/… | the **mixer layer** is off | press `INSERT` |
| a fader key does nothing at first | it is already at that end (fader starts up) | use the opposite key |
| wrong deck responds | `TAB` state | the log line `rbkeyd: active deck N` |
| keys go to the console as well | rbkeyd does not grab | harmless (getty is disabled, fbcon detached — docs/07 §8) |

Note on the multi-interface keyboard: a gaming keyboard exposes several
`/dev/input/eventN` nodes ("Consumer Control", "System Control", "Keyboard").
Only the one advertising A–Z/ENTER/SPACE is used; the keyboard even shows up
with a different node number after each replug, which is why `rbkeyd` rescans
instead of remembering a path.

## 8. Files

```
scripts/rb/device/rbkeyd.c            keyboard -> FIFO daemon (evdev)
scripts/rb/device/fakekbd.c           uinput test keyboard
scripts/rb/deploy-keyboard.sh         build + install + systemd unit
/etc/systemd/system/rbkeyd.service    (on the device)
```
