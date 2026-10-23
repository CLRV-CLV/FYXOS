#!/usr/bin/env python3
"""
FYXOS 磁盘镜像生成工具
======================

把 stage1(MBR) / stage2 / 内核 ELF64 组装成一个可引导的 BIOS 磁盘镜像。

磁盘布局（LBA，512 字节/扇区）：

    LBA 0        stage1（MBR，含引导描述符 + 分区表 + 0xAA55 签名）
    LBA 1..64    stage2（32 KiB，末尾 16 字节为 stage2 描述符）
    LBA 65..     内核 ELF64 映像
    LBA 2048..   FYXOS 系统分区（分区表项 1）

MBR 内 0x1B0 起的 14 字节引导描述符（由本工具填写）：

    +0x0  u32  "FYXB"
    +0x4  u16  stage2 扇区数
    +0x6  u32  内核起始 LBA
    +0xA  u32  内核扇区数

stage2 末尾 16 字节描述符（由本工具填写）：

    +0x0  u32  "FYX2"
    +0x4  u32  内核起始 LBA
    +0x8  u32  内核扇区数
    +0xC  u32  stage2 实际字节数

用法：
    mkimage.py --stage1 build/stage1.bin --stage2 build/stage2.bin \
               --kernel build/fyxos.elf --out build/fyxos.img
"""
import argparse
import os
import struct
import sys

SECTOR_SIZE      = 512
STAGE1_LBA       = 0
STAGE2_LBA       = 1
STAGE2_SECTORS   = 64                       # 32 KiB
KERNEL_LBA       = STAGE2_LBA + STAGE2_SECTORS          # 65
PARTITION_LBA    = 2048                     # 分区从 1 MiB 对齐处开始
STAGE2_MAX_SIZE  = 32752                    # 末尾 16 字节留给描述符
KERNEL_MAX_SECTORS = 2048                   # 内核最多 1 MiB

MBR_DESC_OFF     = 0x1B0
MBR_MAGIC        = b"FYXB"
MBR_PART_TABLE   = 0x1BE
MBR_SIGNATURE    = 0x1FE
STAGE2_DESC_OFF  = STAGE2_MAX_SIZE          # 相对 stage2 映像起点
STAGE2_MAGIC     = b"FYX2"


def fail(msg: str) -> "None":
    print(f"mkimage: 错误: {msg}", file=sys.stderr)
    sys.exit(1)


def read_file(path: str, limit: int = None) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if limit is not None and len(data) > limit:
        fail(f"{path} 过大：{len(data)} > {limit} 字节")
    return data


def lba_to_chs(lba: int) -> bytes:
    """把 LBA 转成 BIOS 分区表用的 CHS（超出容量时返回 0xFE/0xFF/0xFF）"""
    if lba > 1024 * 255 * 63 - 1:
        return bytes([0xFE, 0xFF, 0xFF])
    cylinder = lba // (16 * 63)
    head = (lba // 63) % 16
    sector = lba % 63 + 1
    if cylinder > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    return bytes([head, ((cylinder >> 8) << 6) | sector, cylinder & 0xFF])


def build(args) -> bytes:
    stage1 = read_file(args.stage1, SECTOR_SIZE)
    if len(stage1) != SECTOR_SIZE:
        fail(f"stage1 必须恰好 {SECTOR_SIZE} 字节（实际 {len(stage1)}）")

    stage2 = read_file(args.stage2, STAGE2_MAX_SIZE)
    kernel = read_file(args.kernel, KERNEL_MAX_SECTORS * SECTOR_SIZE)

    kernel_sectors = (len(kernel) + SECTOR_SIZE - 1) // SECTOR_SIZE
    total_sectors = max(args.size_mb * 1024 * 1024 // SECTOR_SIZE,
                        PARTITION_LBA + 4096)

    if KERNEL_LBA + kernel_sectors > PARTITION_LBA:
        fail("内核映像放不进 MBR 与分区之间的空隙（请调小内核或增大分区起始 LBA）")

    img = bytearray(b"\0" * (total_sectors * SECTOR_SIZE))

    # ---------------- stage1 + 引导描述符 + 分区表 ------------------------
    stage1 = bytearray(stage1)
    stage1[MBR_DESC_OFF:MBR_DESC_OFF + 4] = MBR_MAGIC
    struct.pack_into("<H", stage1, MBR_DESC_OFF + 4, STAGE2_SECTORS)
    struct.pack_into("<I", stage1, MBR_DESC_OFF + 6, KERNEL_LBA)
    struct.pack_into("<I", stage1, MBR_DESC_OFF + 10, kernel_sectors)

    if total_sectors <= PARTITION_LBA:
        fail("镜像太小，放不下系统分区")
    part = bytearray(16)
    part[0] = 0x80                                  # 可引导
    part[1:4] = lba_to_chs(PARTITION_LBA)
    part[4] = 0x83                                  # 类型：Linux/FYXOS 数据分区
    part[5:8] = lba_to_chs(total_sectors - 1)
    struct.pack_into("<I", part, 8, PARTITION_LBA)
    struct.pack_into("<I", part, 12, total_sectors - PARTITION_LBA)
    stage1[MBR_PART_TABLE:MBR_PART_TABLE + 16] = part

    stage1[MBR_SIGNATURE] = 0x55
    stage1[MBR_SIGNATURE + 1] = 0xAA

    img[0:SECTOR_SIZE] = stage1

    # ---------------- stage2 + stage2 描述符 ------------------------------
    s2 = bytearray(b"\0" * (STAGE2_SECTORS * SECTOR_SIZE))
    s2[0:len(stage2)] = stage2
    struct.pack_into("<4s", s2, STAGE2_DESC_OFF, STAGE2_MAGIC)
    struct.pack_into("<I", s2, STAGE2_DESC_OFF + 4, KERNEL_LBA)
    struct.pack_into("<I", s2, STAGE2_DESC_OFF + 8, kernel_sectors)
    struct.pack_into("<I", s2, STAGE2_DESC_OFF + 12, len(stage2))
    start = STAGE2_LBA * SECTOR_SIZE
    img[start:start + len(s2)] = s2

    # ---------------- 内核 ELF64 -----------------------------------------
    kstart = KERNEL_LBA * SECTOR_SIZE
    img[kstart:kstart + len(kernel)] = kernel

    return bytes(img), {
        "stage2_size": len(stage2),
        "stage2_sectors": STAGE2_SECTORS,
        "kernel_size": len(kernel),
        "kernel_sectors": kernel_sectors,
        "kernel_lba": KERNEL_LBA,
        "partition_lba": PARTITION_LBA,
        "total_sectors": total_sectors,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="FYXOS 磁盘镜像生成工具")
    ap.add_argument("--stage1", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=32,
                    help="镜像总大小（MiB，默认 32）")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    img, info = build(args)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(img)

    if args.verbose:
        print(f"  stage1      {SECTOR_SIZE:>8} 字节  @ LBA {STAGE1_LBA}")
        print(f"  stage2      {info['stage2_size']:>8} 字节  @ LBA {STAGE2_LBA}"
              f" ({info['stage2_sectors']} 扇区)")
        print(f"  kernel      {info['kernel_size']:>8} 字节  @ LBA "
              f"{info['kernel_lba']} ({info['kernel_sectors']} 扇区)")
        print(f"  partition   @ LBA {info['partition_lba']}"
              f" ({info['total_sectors'] - info['partition_lba']} 扇区)")
    print(f"  镜像 {args.out}：{len(img)} 字节 ({info['total_sectors']} 扇区)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
