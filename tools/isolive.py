#!/usr/bin/env python3
"""FYXOS Live ISO 的共享格式定义与严格解析器。

ISO 的系统使用区（LBA 1）保存一个很小的 FYXOSCD1 描述符。BIOS 仍通过
标准 El Torito hard-disk emulation 进入自研 stage1/stage2；内核脱离 BIOS
后则由 ATAPI 驱动读取该描述符，把光盘中的 FYXOS.IMG 映射为 512 字节块设备。
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

ISO_SECTOR = 2048
LIVE_DESC_LBA = 1
LIVE_MAGIC = b"FYXOSCD1"
LIVE_VERSION = 1
LIVE_STRUCT = struct.Struct("<8sIIQII")
assert LIVE_STRUCT.size == 32


class IsoFormatError(ValueError):
    """ISO 缺少有效的 FYXOS/El Torito 启动描述。"""


@dataclass(frozen=True)
class LiveIsoInfo:
    catalog_lba: int
    image_lba: int
    image_size: int
    image_sectors: int
    image_offset: int
    image_end: int
    image: bytes


def _sector(data: bytes, lba: int) -> bytes:
    start = lba * ISO_SECTOR
    end = start + ISO_SECTOR
    if lba < 0 or end > len(data):
        raise IsoFormatError(f"ISO 缺少 LBA {lba}")
    return data[start:end]


def make_live_descriptor(image_lba: int, image_size: int) -> bytes:
    if image_lba < 16 or image_size <= 0 or image_size % 512:
        raise ValueError("无效的 Live 启动载荷位置或大小")
    image_sectors = image_size // 512
    prefix = LIVE_STRUCT.pack(LIVE_MAGIC, LIVE_VERSION, image_lba,
                              image_size, image_sectors, 0)[:28]
    checksum = zlib.crc32(prefix) & 0xFFFFFFFF
    return LIVE_STRUCT.pack(LIVE_MAGIC, LIVE_VERSION, image_lba,
                            image_size, image_sectors, checksum)


def parse_live_descriptor(data: bytes, cd_sectors: int | None = None) -> tuple[int, int, int]:
    raw = _sector(data, LIVE_DESC_LBA)[:LIVE_STRUCT.size]
    magic, version, image_lba, image_size, image_sectors, checksum = \
        LIVE_STRUCT.unpack(raw)
    if magic != LIVE_MAGIC or version != LIVE_VERSION:
        raise IsoFormatError("不是 FYXOS Live ISO（缺少 FYXOSCD1）")
    if (zlib.crc32(raw[:28]) & 0xFFFFFFFF) != checksum:
        raise IsoFormatError("FYXOSCD1 描述符 CRC32 不匹配")
    if image_size <= 0 or image_size % 512 or image_sectors != image_size // 512:
        raise IsoFormatError("FYXOSCD1 载荷尺寸无效")
    image_offset = image_lba * ISO_SECTOR
    image_end = image_offset + image_size
    if image_lba < 16 or image_offset < 0 or image_end > len(data):
        raise IsoFormatError("FYXOSCD1 载荷超出 ISO 介质")
    if cd_sectors is not None and image_end > cd_sectors * ISO_SECTOR:
        raise IsoFormatError("FYXOSCD1 载荷超出 ATAPI READ CAPACITY")
    return image_lba, image_size, image_sectors


def read_live_iso(data: bytes) -> LiveIsoInfo:
    """验证 BIOS 启动链和内核 ATAPI 视图，并返回内嵌原始磁盘。"""
    if len(data) < 25 * ISO_SECTOR or len(data) % ISO_SECTOR:
        raise IsoFormatError("ISO 大小必须是完整的 2048 字节逻辑块")

    pvd = _sector(data, 16)
    if pvd[0:7] != b"\x01CD001\x01":
        raise IsoFormatError("缺少 ISO9660 Primary Volume Descriptor")
    logical_size = struct.unpack_from("<H", pvd, 128)[0]
    volume_sectors = struct.unpack_from("<I", pvd, 80)[0]
    if logical_size != ISO_SECTOR or volume_sectors * ISO_SECTOR > len(data):
        raise IsoFormatError("ISO9660 卷尺寸或逻辑块尺寸无效")

    boot = _sector(data, 17)
    if boot[0:7] != b"\x00CD001\x01" or \
            not boot[7:39].startswith(b"EL TORITO SPECIFICATION"):
        raise IsoFormatError("缺少 El Torito Boot Record")
    catalog_lba = struct.unpack_from("<I", boot, 71)[0]
    catalog = _sector(data, catalog_lba)
    validation = catalog[:32]
    if validation[0] != 1 or validation[1] != 0 or validation[30:32] != b"\x55\xAA":
        raise IsoFormatError("El Torito Validation Entry 无效")
    if sum(struct.unpack("<16H", validation)) & 0xFFFF:
        raise IsoFormatError("El Torito Validation Entry 校验和无效")
    initial = catalog[32:64]
    if initial[0] != 0x88 or initial[1] != 0x04:
        raise IsoFormatError("默认启动项不是可引导 hard-disk emulation")
    if struct.unpack_from("<H", initial, 6)[0] != 1:
        raise IsoFormatError("BIOS 启动项必须加载一个虚拟扇区")
    catalog_image_lba = struct.unpack_from("<I", initial, 8)[0]

    image_lba, image_size, image_sectors = parse_live_descriptor(
        data, cd_sectors=volume_sectors)
    if catalog_image_lba != image_lba:
        raise IsoFormatError("El Torito 与 FYXOSCD1 指向不同的启动载荷")
    image_offset = image_lba * ISO_SECTOR
    image_end = image_offset + image_size
    image = data[image_offset:image_end]
    if len(image) < 2048 * 512 or image[510:512] != b"\x55\xAA" or \
            image[0x1B0:0x1B4] != b"FYXB":
        raise IsoFormatError("内嵌载荷不是有效 FYXOS BIOS 磁盘")

    part = image[0x1BE:0x1CE]
    part_lba, part_sectors = struct.unpack_from("<II", part, 8)
    if part[4] != 0x83 or part_lba == 0 or \
            part_lba + part_sectors > image_sectors:
        raise IsoFormatError("内嵌 FYXOS 系统分区无效")
    if initial[4] != part[4]:
        raise IsoFormatError("El Torito System Type 与 MBR 分区类型不一致")

    return LiveIsoInfo(catalog_lba=catalog_lba, image_lba=image_lba,
                       image_size=image_size, image_sectors=image_sectors,
                       image_offset=image_offset, image_end=image_end,
                       image=image)
