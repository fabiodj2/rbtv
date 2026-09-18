# 09 — Input working: buttons into `rb` via the keyshim FIFO

`rb` has no real front panel on the Chromebit, so button presses are injected
directly into its key manager from a FIFO. The shim also starts the message pump
that actually dispatches those keys, clears the stale browse caution, and pushes
the mixer defaults.

Status: **working** — verified by injecting the **SOURCE** key, which switched the
UI to the source view (`SOURCE` / `DEVICE NAME` / `NO DEVICE`).

## 1. Mechanism

```
host / script                         (this repo: scripts/rb/device/press-key.sh)
  writes 12-byte records
        { int32 key; int32 ch; int32 down }         (little-endian)
   ▼
  /tmp/rb-keys.fifo      (FIFO; the chroot's /tmp is bind-mounted to the host /tmp)
   ▼
keyshim.so  (LD_PRELOAD inside rbp; a thread polls the FIFO)
  calls IKeyManager::sendKey(keycode, op, ch, param, f, l)
   ▼
rbp key dispatch
```

A second FIFO, **`/tmp/rb-ctrl.fifo`**, carries 24-byte control records for
real control surfaces (faders, EQ, jog speed, knobs) — see docs/12:

```
struct ctrl_ev { int32 key, ch, op, param; float f; int32 l; }
        op 0 press / 2 release / 4 rotate (param = 10-bit absolute or ±delta)
                                 / 5 value  (param = 10-bit, f = normalised)
```

While `rb-ctrl.fifo` is being fed, keyshim stops re-applying the synthetic
mixer defaults (§1 below), so physical faders are not overwritten every 10 s.

Injection uses rbp's own object graph (same binary as the Prime GO, so the
same addresses):
```
IUiObjManager singleton   @ 0x02685f2c
KeyManager                = *(*(0x02685f2c) + 100)
sendKey                   = KeyManager vtable[2]
sendKey(keycode, op, ch, param, f, l)
    op: 0 = press, 2 = release, 4 = rotate, 5 = absolute value
```

`keyshim.so` (ported from rb2go `device/keyshim.c`, same rbp → same addresses)
does more than read the FIFO:

* **starts the `UiMain` pump.** `IKeyManager::sendKey` only *posts* to the
  `PanelComPeerLinux` MsgManager (`mainMsgManager @ 0x0268612c`); the queue is
  drained only by `PanelComPeerLinux::run()` (thread `UiMain`), which is started
  by `openDevice()`. On a device with no sub-MCU that thread never exists, so
  injected keys would sit in the queue forever. keyshim calls `openDevice(0)` /
  `startThread()` to bring it up.
* **clears the stale browse caution** (`[0x05a191fc] = 0x12d`), which otherwise
  gates browse/touch.
* **applies mixer defaults** (and re-applies every 10 s): channel faders up,
  trims/EQ centred, crossfader centred — keycodes `0x501e/0x5019/0x501a-c/0x6017`.
  Needed because the RX3 sub-MCU would normally set the physical positions; with
  no MCU the engine's default faders are at 0 (silence).
* **pins the mixer input routing** (`djengine::MixerRouteMngr`, `0x01149f50`/
  `0x01149f54`). On the RX3 the DECK/LINE switches assign each mixer channel to
  a player; without the sub-MCU **both** channels come up on Player 0, so the
  channel-2 fader moves deck 1 (both faders control the same sound). keyshim
  writes `ch1 -> 0x01149f08` (Player 0/deck 1) and `ch2 -> 0x01149f10`
  (Player 1/deck 2) with the mixer defaults — same as PrimeBox's `knobshim2`
  (PrimeBox `docs/05-controls.md` §5).

## 2. Keycodes

| Name | keycode | Name | keycode |
|---|---|---|---|
| Source | `0x0201` | Select | `0x420c` |
| Browse | `0x0202` | Back | `0x420d` |
| Tag list | `0x0203` | Load deck 1 / 2 | `0x4311` / `0x4312` |
| Menu | `0x0206` | Play deck 1 / 2 | `0x4101` / `0x4102` |
| Link | `0x0207` | Cue deck 1 / 2 | `0x4103` / `0x4104` |
| Rekordbox | `0x0208` | Sync deck 1 / 2 | `0x4112` |
| USB1 | `0x0209` | | |
| Info | `0x020b` | | |

(Full map: rb2go `device/rbviewer.c` `g_kmap[]` / `g_cmap[]`, and PrimeBox
`primego-mapping/MAPPING.md`.)

## 3. Build & deploy

`keyshim.so` is built by `scripts/rb/build-shims.sh` (soft-float, only
`GLIBC_2.4`; md5 `86a9e1a3c8a2e99fd2322d0cea257ec1`) and deployed by
`scripts/rb/deploy-module.sh`. It is loaded last:

```
LD_PRELOAD=/usr/lib/memshim.so:/usr/lib/fbshim.so:/usr/lib/audioshim.so:/usr/lib/keyshim.so
```

## 4. Test

```sh
# on the Chromebit, with rbp running
CHROMEBIT# sh /home/user/press-key.sh source
injecting key=0x0201 ch=0 (press+release) -> /tmp/rb-keys.fifo
done. ...
CHROMEBIT# tail -4 /tmp/keyshim.log
keyshim: got key ch=0
         down=1
sendkey key=00000201
keyshim: got key ch=0
         down=0
sendkey key=00000201
```

A healthy startup log:

```
keyshim: thread started
keyshim: key manager ready km=aaf937b8
keyshim: UiMain pump not running -> openDevice(0)
keyshim: UiMain pump started
keyshim: mixer defaults sent (faders up, trims/EQ/crossfader center)
keyshim: fifo open ok
```

## 5. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `sendkey key=…` logged but UI does nothing | `UiMain` pump not running (keys queue, never dispatched) | keyshim starts it; check `keyshim: UiMain pump started` in the log |
| no `got key` in the log | FIFO not open, or the record arrived in several `write()`s and was discarded | the shim accumulates partial reads (12-byte record); use `press-key.sh` which also writes it in one `write()` |
| `no /tmp/rb-keys.fifo` | rbp not started | `sh /home/user/start-rb.sh` creates it |
| browse/touch appears disabled | stale USB caution id set | keyshim clears `[0x05a191fc]` continuously |
| no sound with a track loaded | mixer defaults not applied (faders at 0) | keyshim re-applies them every 10 s |
| channel-2 fader moves deck 1 (both faders do the same) | mixer input routing stuck on Player 0 (no sub-MCU DECK/LINE switch) | keyshim `mixer_route()`; check `0x01149f50=01149f08`, `0x01149f54=01149f10` |
| keyshim not loaded | `LD_PRELOAD` missing it | `start-rb.sh` includes `/usr/lib/keyshim.so` |

## 6. Files

```
scripts/rb/keyshim.c                FIFO -> sendKey (+ UiMain pump, caution, mixer)
scripts/rb/shims-Makefile           keyshim.so build rule
scripts/rb/build-shims.sh           builds memshim/audioshim/keyshim (soft-float)
scripts/rb/start-rb.sh              LD_PRELOAD order (…:audioshim.so:keyshim.so)
scripts/rb/device/press-key.sh      on-device key injector (names or raw keycode)
scripts/rb/device/ddj400-bridge.c   DDJ-400 MIDI -> /tmp/rb-ctrl.fifo (docs/12)
```
