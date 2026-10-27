#!/usr/bin/env python3
"""Create a guest negative fixture: valid ar/control, corrupted data.tar.gz CRC32."""
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    data = bytearray(args.input.read_bytes())
    if not data.startswith(b"!<arch>\n"):
        raise RuntimeError("input is not ar")
    offset = 8
    found = False
    while offset < len(data):
        if offset + 60 > len(data):
            raise RuntimeError("truncated ar header")
        header = bytes(data[offset:offset + 60])
        name = header[:16].decode("ascii").rstrip().rstrip("/")
        size = int(header[48:58].decode("ascii").strip())
        start = offset + 60
        end = start + size
        if end > len(data):
            raise RuntimeError("truncated ar member")
        if name == "data.tar.gz":
            if size < 18 or data[start:start + 3] != b"\x1f\x8b\x08":
                raise RuntimeError("data member is not gzip")
            data[end - 8] ^= 1       # CRC32 mismatch; DEFLATE bytes stay valid
            found = True
            break
        offset = end + (size & 1)
    if not found:
        raise RuntimeError("data.tar.gz not found")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"  DEB-NEG {args.output} ({len(data)} bytes, bad data CRC32)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
