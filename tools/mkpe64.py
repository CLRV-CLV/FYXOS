#!/usr/bin/env python3
"""Wrap a resolved GNU-as .text section in a minimal, standard Win64 PE32+ file."""
from __future__ import annotations

import argparse
import struct
import subprocess
from pathlib import Path

IMAGE_BASE = 0x400000
SECTION_RVA = 0x1000
IMPORT_RVA = 0x1400
FILE_ALIGNMENT = 0x200
SECTION_ALIGNMENT = 0x1000


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--object", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    section = subprocess.check_output([
        "objcopy", "-O", "binary", "--only-section=.text", str(args.object),
        "/dev/stdout",
    ])
    if len(section) < IMPORT_RVA - SECTION_RVA + 40:
        raise RuntimeError("section is shorter than its import directory")
    if len(section) >= SECTION_ALIGNMENT:
        raise RuntimeError("demo section must stay below one page")

    raw_size = align(len(section), FILE_ALIGNMENT)
    size_of_image = SECTION_RVA + align(len(section), SECTION_ALIGNMENT)

    dos = bytearray(0x80)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, len(dos))

    coff = bytearray(20)
    struct.pack_into("<HHIIIHH", coff, 0,
                     0x8664, 1, 0, 0, 0, 240, 0x0023)

    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)       # PE32+
    optional[2] = 1
    struct.pack_into("<III", optional, 4, raw_size, 0, 0)
    struct.pack_into("<III", optional, 16, SECTION_RVA, SECTION_RVA, 0)
    struct.pack_into("<Q", optional, 24, IMAGE_BASE)
    struct.pack_into("<II", optional, 32, SECTION_ALIGNMENT, FILE_ALIGNMENT)
    struct.pack_into("<HHHH", optional, 40, 6, 0, 0, 0)
    struct.pack_into("<HH", optional, 48, 6, 0)
    struct.pack_into("<I", optional, 52, 0)
    struct.pack_into("<II", optional, 56, size_of_image, FILE_ALIGNMENT)
    struct.pack_into("<I", optional, 64, 0)           # optional PE checksum
    struct.pack_into("<HH", optional, 68, 3, 0x0100)  # CUI, NX_COMPAT
    struct.pack_into("<QQQQ", optional, 72,
                     0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<II", optional, 104, 0, 16)
    struct.pack_into("<II", optional, 112 + 8, IMPORT_RVA, 40)

    header = bytearray(40)
    header[:8] = b".text\0\0\0"
    struct.pack_into("<IIIIIIHHI", header, 8,
                     len(section), SECTION_RVA, raw_size, FILE_ALIGNMENT,
                     0, 0, 0, 0, 0xE0000020)  # CODE|EXECUTE|READ|WRITE

    headers = dos + b"PE\0\0" + coff + optional + header
    if len(headers) > FILE_ALIGNMENT:
        raise RuntimeError("PE headers exceed file alignment")
    headers += bytes(FILE_ALIGNMENT - len(headers))
    output = headers + section + bytes(raw_size - len(section))
    args.output.write_bytes(output)
    print(f"  PE32+   {args.output} ({len(output)} bytes, imports @ {IMPORT_RVA:#x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
