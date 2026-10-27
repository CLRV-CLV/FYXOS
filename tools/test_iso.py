#!/usr/bin/env python3
"""FYXOS Live ISO 结构、完整 BIOS/ATAPI 启动与 RAM overlay 验收。"""
from __future__ import annotations

import argparse
import binascii
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from isolive import ISO_SECTOR, IsoFormatError, read_live_iso

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"
LIVE_FILE = "/home/user/iso-live.txt"
LIVE_TEXT = "FYX_ISO_LIVE_OK"


class IsoTest:
    def __init__(self, iso: str, image: str, kernel: str, work: str,
                 max_seconds: int):
        self.iso = Path(iso).resolve()
        self.image = Path(image).resolve()
        self.kernel = Path(kernel).resolve()
        self.work = Path(work).resolve()
        self.max_seconds = max_seconds
        self.data = self.iso.read_bytes()
        self.info = read_live_iso(self.data)
        self.passes = 0
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)

    def passed(self, label: str):
        self.passes += 1
        print(f"  [PASS] {label}")

    @staticmethod
    def require(condition: bool, label: str):
        if not condition:
            raise RuntimeError(label)

    def expect_reject(self, label: str, data: bytes):
        try:
            read_live_iso(data)
        except IsoFormatError:
            self.passed(label)
            return
        raise RuntimeError(f"损坏 ISO 未被拒绝：{label}")

    def structure(self):
        self.require(self.info.image == self.image.read_bytes(),
                     "ISO 内嵌 IMG 与构建输入不一致")
        self.passed("ISO9660/El Torito/FYXOSCD1 与内嵌 IMG 完全一致")

        pvd = self.data[16 * ISO_SECTOR:17 * ISO_SECTOR]
        root_lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
        root_size = struct.unpack_from("<I", pvd, 156 + 10)[0]
        root = self.data[root_lba * ISO_SECTOR:root_lba * ISO_SECTOR + root_size]
        files = {}
        pos = 0
        while pos + 34 <= len(root) and root[pos]:
            length = root[pos]
            self.require(length >= 34 and pos + length <= len(root),
                         "根目录记录越界")
            name_len = root[pos + 32]
            self.require(33 + name_len <= length, "根目录文件名越界")
            name = bytes(root[pos + 33:pos + 33 + name_len])
            extent = struct.unpack_from("<I", root, pos + 2)[0]
            size = struct.unpack_from("<I", root, pos + 10)[0]
            files[name] = (extent, size)
            pos += length
        self.require(files.get(b"FYXOS.IMG;1") ==
                     (self.info.image_lba, self.info.image_size),
                     "根目录 FYXOS.IMG;1 extent 错误")
        self.require(b"README.TXT;1" in files and b"\x00" in files and
                     b"\x01" in files, "根目录记录不完整")
        self.passed("ISO9660 根目录、README 与启动文件 extent 可遍历")

        mutated = bytearray(self.data)
        mutated[16 * ISO_SECTOR + 1] ^= 1
        self.expect_reject("拒绝损坏的 PVD CD001 签名", bytes(mutated))

        mutated = bytearray(self.data)
        mutated[self.info.catalog_lba * ISO_SECTOR + 4] ^= 1
        self.expect_reject("拒绝 El Torito validation checksum 错误", bytes(mutated))

        mutated = bytearray(self.data)
        struct.pack_into("<I", mutated, self.info.catalog_lba * ISO_SECTOR + 32 + 8,
                         self.info.image_lba + 1)
        self.expect_reject("拒绝 catalog 与 Live 描述符载荷不一致", bytes(mutated))

        mutated = bytearray(self.data)
        mutated[ISO_SECTOR + 28] ^= 1
        self.expect_reject("拒绝 FYXOSCD1 CRC32 错误", bytes(mutated))
        self.expect_reject("拒绝截断的光盘介质", self.data[:-ISO_SECTOR])

    def boot(self, name: str, keys: str) -> str:
        log_path = self.work / f"{name}.log"
        cmd = [
            sys.executable, str(ROOT / "tools/fyxemu.py"),
            "--iso", str(self.iso), "--kernel", str(self.kernel),
            "--mode", "full-boot", "--keys", keys, "--keys-after", READY,
            "--max-seconds", str(self.max_seconds),
        ]
        with log_path.open("wb") as stream:
            result = subprocess.run(cmd, cwd=ROOT, stdout=stream,
                                    stderr=subprocess.STDOUT,
                                    timeout=self.max_seconds + 30)
        text = log_path.read_bytes().decode("utf-8", "replace")
        if result.returncode != 0 or "正常关机" not in text:
            raise RuntimeError(f"{name}: exit={result.returncode}\n{text[-6000:]}")
        if re.search(r"\[panic\]|kernel panic|内核 panic|CPU 异常", text, re.I):
            raise RuntimeError(f"{name}: 出现 panic/CPU 异常")
        self.require("[block] ATAPI Live CD" in text and READY in text,
                     f"{name}: 未经过 ATAPI 进入完整用户态")
        return text

    def runtime(self):
        before = hashlib.sha256(self.data).hexdigest()
        out = self.boot(
            "live-session",
            f"write {LIVE_FILE} {LIVE_TEXT};cat {LIVE_FILE};sync;"
            f"unmount;mount;cat {LIVE_FILE};cp {LIVE_FILE} /tmp/iso-live-copy;"
            f"stat {LIVE_FILE};stat /tmp/iso-live-copy;fsck;df;shutdown")
        payload = (LIVE_TEXT + "\n").encode("ascii")
        crc = binascii.crc32(payload) & 0xFFFFFFFF
        original_stat = rf"{LIVE_FILE}: node=\d+ type=file size={len(payload)} crc32=0x{crc:08x}"
        copy_stat = rf"/tmp/iso-live-copy: node=\d+ type=file size={len(payload)} crc32=0x{crc:08x}"
        self.require(re.search(original_stat, out) is not None and
                     re.search(copy_stat, out) is not None,
                     "Live 文件在 unmount/mount 后读回内容的尺寸/CRC32 不符")
        self.require("[storage-write] /home/user/iso-live.txt" in out and
                     "Live RAM overlay" in out,
                     "Live 写入/同步标记缺失")
        self.require(re.search(r"\[fsck\][^\n]*=0", out) is not None,
                     "Live overlay 文件系统 fsck 未通过")
        self.passed("完整 BIOS→ATAPI→ring3；Live 写入、重挂、读回与 fsck 通过")

        out = self.boot("live-reboot", f"cat {LIVE_FILE};fsck;shutdown")
        self.require(f"cat: {LIVE_FILE}:" in out and " -8" in out and
                     LIVE_TEXT not in out,
                     "重启后上一会话 Live 文件仍然存在")
        self.passed("同一 ISO 重启后上一会话文件消失（易失性语义真实）")
        after = hashlib.sha256(self.iso.read_bytes()).hexdigest()
        self.require(after == before, "模拟运行修改了原始 ISO")
        self.passed(f"两次完整启动未改写 ISO（SHA256 {after}）")

    def run_all(self):
        self.structure()
        self.runtime()
        print(f"ISO 专项全部通过：{self.passes} PASS / 0 FAIL；日志 {self.work}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default=str(ROOT / "build/fyxos.iso"))
    ap.add_argument("--image", default=str(ROOT / "build/fyxos.img"))
    ap.add_argument("--kernel", default=str(ROOT / "build/fyxos.elf"))
    ap.add_argument("--work", default=str(ROOT / "build/iso-test"))
    ap.add_argument("--max-seconds", type=int, default=90)
    args = ap.parse_args()
    try:
        IsoTest(args.iso, args.image, args.kernel, args.work,
                args.max_seconds).run_all()
    except (RuntimeError, OSError, IsoFormatError, subprocess.TimeoutExpired) as exc:
        print(f"iso-test: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
