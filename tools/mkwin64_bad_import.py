#!/usr/bin/env python3
"""Keep a PE32+ structurally valid but rename one import to an unsupported API."""
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    data = bytearray(args.input.read_bytes())
    if data.count(b"WriteFile\0") != 1:
        raise RuntimeError("expected exactly one WriteFile import")
    start = data.index(b"WriteFile\0")
    data[start:start + 9] = b"XriteFile"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"  PE-NEG  {args.output} (unknown kernel32!XriteFile)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
