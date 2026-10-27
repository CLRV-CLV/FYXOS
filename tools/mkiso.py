#!/usr/bin/env python3
"""不依赖外部 mkisofs/xorriso 的 FYXOS BIOS Live ISO 生成器。"""
from __future__ import annotations

import argparse
import hashlib
import os
from datetime import datetime, timezone
import struct
import sys

from isolive import (ISO_SECTOR, LIVE_DESC_LBA, IsoFormatError,
                     make_live_descriptor, read_live_iso)

CATALOG_LBA = 19
PATH_TABLE_LBA = 20
PATH_TABLE_M_LBA = 21
ROOT_LBA = 22
IMAGE_LBA = 24
BOOT_SYSTEM_ID = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")


def _both16(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def _both32(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def _volume_time(epoch: int) -> bytes:
    dt = datetime.fromtimestamp(epoch, timezone.utc)
    return dt.strftime("%Y%m%d%H%M%S00").encode("ascii") + b"\x00"


def _record_time(epoch: int) -> bytes:
    dt = datetime.fromtimestamp(epoch, timezone.utc)
    return bytes((dt.year - 1900, dt.month, dt.day, dt.hour, dt.minute,
                  dt.second, 0))


def _dir_record(extent: int, size: int, flags: int, identifier: bytes,
                epoch: int) -> bytes:
    length = 33 + len(identifier) + (1 if len(identifier) % 2 == 0 else 0)
    out = bytearray(length)
    out[0] = length
    out[2:10] = _both32(extent)
    out[10:18] = _both32(size)
    out[18:25] = _record_time(epoch)
    out[25] = flags
    out[28:32] = _both16(1)
    out[32] = len(identifier)
    out[33:33 + len(identifier)] = identifier
    return bytes(out)


def _catalog() -> bytes:
    out = bytearray(ISO_SECTOR)
    validation = bytearray(32)
    validation[0] = 1                    # Validation Entry
    validation[1] = 0                    # 80x86
    validation[4:28] = b"FYXOS BIOS LIVE".ljust(24, b" ")
    validation[30:32] = b"\x55\xAA"
    checksum = (-sum(struct.unpack("<16H", validation))) & 0xFFFF
    struct.pack_into("<H", validation, 28, checksum)
    out[:32] = validation

    initial = bytearray(32)
    initial[0] = 0x88                    # bootable
    initial[1] = 0x04                    # hard-disk emulation
    struct.pack_into("<H", initial, 2, 0)  # default load segment 0x07C0
    initial[4] = 0x83                    # 与内嵌 MBR 分区 System Type 一致
    struct.pack_into("<H", initial, 6, 1)  # one virtual 512-byte sector
    struct.pack_into("<I", initial, 8, IMAGE_LBA)
    out[32:64] = initial
    return bytes(out)


def _pvd(total_sectors: int, root_record: bytes, epoch: int) -> bytes:
    out = bytearray(ISO_SECTOR)
    out[0:7] = b"\x01CD001\x01"
    out[8:40] = b"FYXOS".ljust(32, b" ")
    out[40:72] = b"FYXOS_LIVE".ljust(32, b" ")
    out[80:88] = _both32(total_sectors)
    out[120:124] = _both16(1)
    out[124:128] = _both16(1)
    out[128:132] = _both16(ISO_SECTOR)
    out[132:140] = _both32(10)           # one root path-table record
    struct.pack_into("<I", out, 140, PATH_TABLE_LBA)
    struct.pack_into("<I", out, 144, 0)
    struct.pack_into(">I", out, 148, PATH_TABLE_M_LBA)
    struct.pack_into(">I", out, 152, 0)
    out[156:156 + len(root_record)] = root_record
    out[190:318] = b"FYXOS_LIVE".ljust(128, b" ")
    out[318:446] = b"FYXOS PROJECT".ljust(128, b" ")
    out[446:574] = b"FYXOS PROJECT".ljust(128, b" ")
    out[574:702] = b"FYXOS SELF-CONTAINED MKISO".ljust(128, b" ")
    out[702:739] = b" ".ljust(37, b" ")
    out[739:776] = b" ".ljust(37, b" ")
    out[776:813] = b" ".ljust(37, b" ")
    created = _volume_time(epoch)
    out[813:830] = created
    out[830:847] = created
    out[847:864] = b"0" * 16 + b"\x00"
    out[864:881] = created
    out[881] = 1
    out[883:1395] = b"FYXOS BIOS LIVE; RAM OVERLAY; NOT UEFI".ljust(512, b"\x00")
    return bytes(out)


def build_iso(image: bytes, readme: bytes, epoch: int = 0) -> bytes:
    if len(image) < 2048 * 512 or len(image) % ISO_SECTOR:
        raise ValueError("FYXOS Live 载荷至少需要 1 MiB 且按 2048 字节对齐")
    if image[510:512] != b"\x55\xAA" or image[0x1B0:0x1B4] != b"FYXB":
        raise ValueError("输入不是 mkimage.py 生成的 FYXOS BIOS 磁盘")
    if len(image) // 512 > 65536:
        raise ValueError("BIOS hard-disk emulation 载荷超过 32 MiB 上限")

    image_sectors = len(image) // ISO_SECTOR
    readme_lba = IMAGE_LBA + image_sectors
    if len(readme) > 64 * ISO_SECTOR:
        raise ValueError("ISO 内 README 超过 128 KiB")
    readme_sectors = max(1, (len(readme) + ISO_SECTOR - 1) // ISO_SECTOR)
    total_sectors = readme_lba + readme_sectors
    root_record = _dir_record(ROOT_LBA, ISO_SECTOR, 2, b"\x00", epoch)

    iso = bytearray(total_sectors * ISO_SECTOR)
    desc = make_live_descriptor(IMAGE_LBA, len(image))
    desc_off = LIVE_DESC_LBA * ISO_SECTOR
    iso[desc_off:desc_off + len(desc)] = desc
    iso[16 * ISO_SECTOR:17 * ISO_SECTOR] = _pvd(total_sectors, root_record, epoch)

    boot = bytearray(ISO_SECTOR)
    boot[0:7] = b"\x00CD001\x01"
    boot[7:39] = BOOT_SYSTEM_ID
    struct.pack_into("<I", boot, 71, CATALOG_LBA)
    iso[17 * ISO_SECTOR:18 * ISO_SECTOR] = boot
    iso[18 * ISO_SECTOR:19 * ISO_SECTOR] = b"\xFFCD001\x01".ljust(
        ISO_SECTOR, b"\x00")
    iso[CATALOG_LBA * ISO_SECTOR:(CATALOG_LBA + 1) * ISO_SECTOR] = _catalog()

    path_le = bytearray(10)
    path_le[0] = 1
    struct.pack_into("<I", path_le, 2, ROOT_LBA)
    struct.pack_into("<H", path_le, 6, 1)
    path_be = bytearray(10)
    path_be[0] = 1
    struct.pack_into(">I", path_be, 2, ROOT_LBA)
    struct.pack_into(">H", path_be, 6, 1)
    path_le_off = PATH_TABLE_LBA * ISO_SECTOR
    path_be_off = PATH_TABLE_M_LBA * ISO_SECTOR
    iso[path_le_off:path_le_off + len(path_le)] = path_le
    iso[path_be_off:path_be_off + len(path_be)] = path_be

    root = bytearray()
    root += _dir_record(ROOT_LBA, ISO_SECTOR, 2, b"\x00", epoch)
    root += _dir_record(ROOT_LBA, ISO_SECTOR, 2, b"\x01", epoch)
    root += _dir_record(IMAGE_LBA, len(image), 0, b"FYXOS.IMG;1", epoch)
    root += _dir_record(readme_lba, len(readme), 0, b"README.TXT;1", epoch)
    if len(root) > ISO_SECTOR:
        raise AssertionError("root directory overflow")
    root_off = ROOT_LBA * ISO_SECTOR
    iso[root_off:root_off + len(root)] = root
    image_off = IMAGE_LBA * ISO_SECTOR
    iso[image_off:image_off + len(image)] = image
    readme_off = readme_lba * ISO_SECTOR
    iso[readme_off:readme_off + len(readme)] = readme

    # 生成后必须重新走与模拟器/内核相同的严格解析路径，不能“写了就算”。
    parsed = read_live_iso(bytes(iso))
    if parsed.image != image:
        raise AssertionError("ISO 内嵌启动载荷与输入镜像不一致")
    return bytes(iso)


def main() -> int:
    ap = argparse.ArgumentParser(description="FYXOS BIOS Live ISO 生成器")
    ap.add_argument("--image", required=True, help="mkimage.py 生成的原始 IMG")
    ap.add_argument("--output", required=True)
    ap.add_argument("--readme", help="写入 ISO 根目录 README.TXT;1 的文本")
    args = ap.parse_args()
    with open(args.image, "rb") as f:
        image = f.read()
    if args.readme:
        with open(args.readme, "rb") as f:
            readme = f.read()
    else:
        readme = (b"FYXOS BIOS Live ISO. Use Legacy BIOS, not UEFI.\n"
                  b"Writes use a RAM overlay and are lost after reboot.\n")
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    try:
        iso = build_iso(image, readme, epoch)
    except (ValueError, IsoFormatError) as exc:
        print(f"mkiso: {exc}", file=sys.stderr)
        return 1
    directory = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(directory, exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(iso)
    digest = hashlib.sha256(iso).hexdigest()
    print(f"  ISO     {args.output} ({len(iso)} 字节, "
          f"El Torito BIOS hard-disk emulation)")
    print(f"          载荷 ISO LBA={IMAGE_LBA}, SHA256={digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
