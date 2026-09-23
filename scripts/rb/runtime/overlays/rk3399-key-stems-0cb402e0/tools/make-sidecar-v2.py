#!/usr/bin/env python3
"""Create an RX3STM2 sidecar containing DRUMS followed by VOCAL."""

import argparse
import importlib.util
import json
import os
import pathlib
import struct
import tempfile


HEADER = struct.Struct("<8sIIIIQ32s")
MAGIC_V1 = b"RX3STM1\0"
MAGIC_V2 = b"RX3STM2\0"


def load_encoder(source: pathlib.Path):
    module_path = source / "tools/rx3_stems/sidecar.py"
    spec = importlib.util.spec_from_file_location(
        "rx3_sidecar_v1", module_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {module_path}")

    module = importlib.util.module_from_spec(spec)
    import sys
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def read_v1(path: pathlib.Path):
    with path.open("rb") as stream:
        raw_header = stream.read(HEADER.size)
        if len(raw_header) != HEADER.size:
            raise ValueError(f"truncated temporary sidecar: {path}")

        fields = HEADER.unpack(raw_header)
        magic, rate, channels, fmt, header_size, frames, reserved = fields

        if magic != MAGIC_V1:
            raise ValueError(f"unexpected temporary magic: {magic!r}")
        if header_size != HEADER.size:
            raise ValueError("unexpected temporary header size")

        payload = stream.read()

    frame_size = {1: 8, 2: 4}.get(fmt)
    if frame_size is None:
        raise ValueError(f"unsupported sample format id: {fmt}")
    if len(payload) != frames * frame_size:
        raise ValueError(
            f"payload size mismatch: {len(payload)} != "
            f"{frames * frame_size}"
        )

    return {
        "rate": rate,
        "channels": channels,
        "format": fmt,
        "frames": frames,
        "reserved": reserved,
        "payload": payload,
        "frame_size": frame_size,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--full", required=True, type=pathlib.Path)
    parser.add_argument("--drums", required=True, type=pathlib.Path)
    parser.add_argument("--vocal", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--format", choices=("s16", "f32"), default="s16")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--separator-normalization", type=float)
    args = parser.parse_args()

    encoder = load_encoder(args.source)

    with tempfile.TemporaryDirectory(prefix="rx3-stems-v2-") as directory:
        temporary = pathlib.Path(directory)
        drums_v1 = temporary / "drums.rx3stem"
        vocal_v1 = temporary / "vocal.rx3stem"

        drums_result = encoder.write_sidecar(
            args.drums,
            drums_v1,
            ffmpeg=args.ffmpeg,
            sample_format=args.format,
            match_full=args.full,
            separator_normalization=args.separator_normalization,
        )
        vocal_result = encoder.write_sidecar(
            args.vocal,
            vocal_v1,
            ffmpeg=args.ffmpeg,
            sample_format=args.format,
            match_full=args.full,
            separator_normalization=args.separator_normalization,
        )

        drums = read_v1(drums_v1)
        vocal = read_v1(vocal_v1)

        comparable = ("rate", "channels", "format", "frames", "frame_size")
        for field in comparable:
            if drums[field] != vocal[field]:
                raise ValueError(
                    f"DRUMS/VOCAL mismatch in {field}: "
                    f"{drums[field]} != {vocal[field]}"
                )

        header = HEADER.pack(
            MAGIC_V2,
            drums["rate"],
            drums["channels"],
            drums["format"],
            HEADER.size,
            drums["frames"],
            b"\0" * 32,
        )

        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary_output = args.output.with_name(
            args.output.name + f".tmp.{os.getpid()}"
        )

        try:
            with temporary_output.open("wb") as destination:
                destination.write(header)
                destination.write(drums["payload"])
                destination.write(vocal["payload"])

            expected = (
                HEADER.size +
                2 * drums["frames"] * drums["frame_size"]
            )
            actual = temporary_output.stat().st_size
            if actual != expected:
                raise ValueError(
                    f"RX3STM2 size mismatch: {actual} != {expected}"
                )

            temporary_output.replace(args.output)
        finally:
            if temporary_output.exists():
                temporary_output.unlink()

    print(json.dumps({
        "output": str(args.output),
        "magic": "RX3STM2",
        "sample_rate": drums["rate"],
        "channels": drums["channels"],
        "format": args.format,
        "frames": drums["frames"],
        "seconds": drums["frames"] / drums["rate"],
        "drums_bytes": len(drums["payload"]),
        "vocal_bytes": len(vocal["payload"]),
        "drums_gain": drums_result.gain,
        "vocal_gain": vocal_result.gain,
        "drums_delay": drums_result.delay,
        "vocal_delay": vocal_result.delay,
        "drums_aligned": drums_result.aligned,
        "vocal_aligned": vocal_result.aligned,
    }, indent=2))


if __name__ == "__main__":
    main()
