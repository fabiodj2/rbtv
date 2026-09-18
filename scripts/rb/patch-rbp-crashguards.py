#!/usr/bin/env python3
"""patch-rbp-crashguards.py — Chromebit crash guards for the patched `rbp`.

The PrimeBox patch set (`rbp-audio`, md5 3706c68f…) turns the stock XDJ-RX3
`rbp` into a working player on a non-Pioneer device, but on the Chromebit it
still SIGSEGVs ~1 s after start in the JUCE `NetworkMonitor` timer:

    CRASH pc=0x0031df70 addr=0x0000009c lr=0x003921a0  r0-r3=0

rb2go documented and fixed the same crash (`ui::IUiObjManager::getPcController()`
dereferences a NULL singleton).  rb2go's `patch-rbp-debug.py` bundles a *second*
workaround that the Chromebit does **not** want:

    0x63A44  playengine::Player::getTotalLength() -> always return the "no data"
             sentinel (-259)

That second patch is a phone-specific workaround for a bad-vtable call in the
`Ui_CycleTask` stat poll (rb2go docs/04).  On the Chromebit the stock call is
fine, and forcing `getTotalLength()` to the sentinel breaks the player:
the deck never reports a duration, so the transport/analysis never becomes
ready (press PLAY does nothing, the scrolling waveform in the middle stays
blank).  PrimeBox's working `rbp-audio` does not have it.

This script therefore applies **only the getPcController NULL guard** by
default, and keeps the getTotalLength workaround behind `--with-length-quirk`
for reference / other hardware.

Usage:
    ./patch-rbp-crashguards.py --in rbp-chromebit --out rbp-chromebit
    ./patch-rbp-crashguards.py --check rbp-chromebit
"""
import argparse
import hashlib
import struct
import sys

LOAD_BIAS = 0x8000

# (VA, stock_word, patched_word, note)
GETPC_PATCHES = [
    (0x31DF64, 0xE30636B0, 0xE3A00000, "getPcController: mov r0,#0"),
    (0x31DF68, 0xE3403268, 0xE12FFF1E, "getPcController: bx lr (return NULL)"),
]

LENGTH_PATCHES = [
    (0x63A44, 0xE5933038, 0xEA000001,
     "getTotalLength -> 'no data' sentinel (phone workaround; breaks the deck)"),
]


def md5(data):
    return hashlib.md5(data).hexdigest()


def apply(data, sites):
    applied = already = 0
    for va, old, new, note in sites:
        off = va - LOAD_BIAS
        if off < 0 or off + 4 > len(data):
            sys.exit(f"VA 0x{va:06x} outside file")
        cur = struct.unpack_from("<I", data, off)[0]
        if cur == new:
            already += 1
            continue
        if cur != old:
            sys.exit(f"mismatch at VA 0x{va:06x}: expected 0x{old:08x}, "
                     f"found 0x{cur:08x} ({note}) — wrong rbp version or "
                     f"already modified by another tool")
        struct.pack_into("<I", data, off, new)
        applied += 1
    return applied, already


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", help="input rbp")
    ap.add_argument("--out", dest="out", help="output rbp")
    ap.add_argument("--check", metavar="RBP", help="verify a build only")
    ap.add_argument("--with-length-quirk", action="store_true",
                    help="also apply the phone getTotalLength workaround "
                         "(NOT wanted on the Chromebit)")
    args = ap.parse_args()

    sites = GETPC_PATCHES + (LENGTH_PATCHES if args.with_length_quirk else [])

    if args.check:
        data = open(args.check, "rb").read()
        ok = True
        for va, old, new, note in sites:
            off = va - LOAD_BIAS
            cur = struct.unpack_from("<I", data, off)[0]
            if cur != new:
                print(f"  MISSING @0x{va:06x}: {cur:08x} (want {new:08x})  {note}")
                ok = False
        print(f"{args.check}: md5 {md5(data)}")
        print("check:", "OK" if ok else "INCOMPLETE")
        return 0 if ok else 1

    if not args.src:
        ap.error("--in is required (or use --check)")

    data = bytearray(open(args.src, "rb").read())
    print(f"in : {args.src}  md5 {md5(data)}  ({len(data)} bytes)")
    applied, already = apply(data, sites)
    print(f"[+] patches: {applied} applied, {already} already present, "
          f"{len(sites)} total")

    out = args.out or args.src
    open(out, "wb").write(data)
    print(f"out: {out}  md5 {md5(data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
