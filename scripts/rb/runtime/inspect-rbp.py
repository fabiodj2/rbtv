#!/usr/bin/env python3
"""Read-only compatibility report for an rbp binary and patch manifest."""

import argparse
import hashlib
import json
from pathlib import Path
import sys


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def inspect(binary_path, manifest_path):
    data = binary_path.read_bytes()
    spec = json.loads(manifest_path.read_text())
    rows = []
    for patch in spec.get("patches", []):
        offset = patch["offset"]
        expected = bytes.fromhex(patch["before"])
        actual = data[offset:offset + len(expected)] if offset >= 0 else b""
        rows.append({
            "offset": offset,
            "label": patch.get("label", "unnamed"),
            "expected": expected.hex(),
            "actual": actual.hex(),
            "in_bounds": offset >= 0 and offset + len(expected) <= len(data),
            "matches": actual == expected,
        })
    configured = spec.get("source_sha1", "").lower()
    actual_hash = sha1(data)
    return {
        "binary": str(binary_path),
        "size": len(data),
        "sha1": actual_hash,
        "manifest": str(manifest_path),
        "configured_sha1": configured,
        "sha1_approved": len(configured) == 40 and configured == actual_hash,
        "guards_total": len(rows),
        "guards_matching": sum(row["matches"] for row in rows),
        "guards": rows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rbp", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        report = inspect(args.rbp, args.manifest)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        parser.exit(2, f"ERROR: {error}\n")
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"binary: {report['binary']}")
        print(f"size: {report['size']}")
        print(f"SHA-1: {report['sha1']}")
        print(f"SHA approved: {'YES' if report['sha1_approved'] else 'NO'}")
        for row in report["guards"]:
            state = "MATCH" if row["matches"] else "MISMATCH"
            print(f"{state:8} offset={row['offset']:8} expected={row['expected']} "
                  f"actual={row['actual']} label={row['label']}")
        print(f"guards: {report['guards_matching']}/{report['guards_total']} match")
    return 0 if report["sha1_approved"] and report["guards_matching"] == report["guards_total"] else 1


if __name__ == "__main__":
    sys.exit(main())
