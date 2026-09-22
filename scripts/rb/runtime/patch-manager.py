#!/usr/bin/env python3
"""Offline, guarded RX3 patch staging. Never edits its source or launches rbp."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def digest(data):
    return hashlib.sha1(data).hexdigest()


def stage(source, manifest, output):
    spec = json.loads(manifest.read_text())
    raw = source.read_bytes()
    expected_hash = spec.get("source_sha1", "")
    if len(expected_hash) != 40 or digest(raw) != expected_hash.lower():
        raise ValueError(f"unsupported rbp SHA-1 {digest(raw)}; expected {expected_hash or '(unset)'}")
    patches = spec.get("patches", [])
    if not patches:
        raise ValueError("empty patch list")
    touched = set()
    for patch in patches:
        offset = patch["offset"]
        before = bytes.fromhex(patch["before"])
        after = bytes.fromhex(patch["after"])
        if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0 or not before or len(before) != len(after):
            raise ValueError(f"invalid patch: {patch}")
        span = set(range(offset, offset + len(before)))
        if offset + len(before) > len(raw) or touched.intersection(span):
            raise ValueError(f"out of bounds or overlapping patch at {offset}")
        touched.update(span)
        if raw[offset:offset + len(before)] != before:
            raise ValueError(f"guard mismatch at {offset}: expected {before.hex()}, found {raw[offset:offset + len(before)].hex()}")
    if source.resolve() == output.resolve() or manifest.resolve() == output.resolve() or output.exists():
        raise ValueError("output must be a new path, distinct from source and manifest")
    changed = bytearray(raw)
    for patch in patches:
        after = bytes.fromhex(patch["after"])
        offset = patch["offset"]
        changed[offset:offset + len(after)] = after
    output.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".rx3-patch-", dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(changed)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(tmp, source.stat().st_mode & 0o777)
        if output.exists():
            raise ValueError("output appeared during staging")
        os.replace(tmp, output)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    return digest(changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["verify", "stage", "restore"])
    parser.add_argument("--source", type=Path, required=True, help="immutable stock rbp")
    parser.add_argument("--manifest", type=Path, help="JSON patch manifest, with source_sha1")
    parser.add_argument("--output", type=Path, help="new staged rbp path")
    args = parser.parse_args()
    try:
        if args.action == "restore":
            if not args.output or args.output.exists() or args.source.resolve() == args.output.resolve():
                raise ValueError("restore needs a new output path distinct from source")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.source, args.output)
            print(f"restored copy: {digest(args.output.read_bytes())}")
        else:
            if not args.manifest:
                raise ValueError("--manifest required")
            if args.action == "verify":
                import tempfile as _tempfile
                with _tempfile.TemporaryDirectory() as directory:
                    print(f"validated; staged SHA-1: {stage(args.source, args.manifest, Path(directory) / 'rbp')}")
            else:
                if not args.output:
                    raise ValueError("--output required")
                print(f"staged SHA-1: {stage(args.source, args.manifest, args.output)}")
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        parser.exit(1, f"STOP: {error}\n")


if __name__ == "__main__":
    main()
