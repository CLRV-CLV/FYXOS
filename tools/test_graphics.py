#!/usr/bin/env python3
"""阶段 J 图形交互、PNG 与无 BGA 救援路径验收。"""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def run_case(name, kernel, output_dir, extra, keys, max_seconds, test=False):
    png = output_dir / (name + ".png")
    cmd = [sys.executable, str(ROOT / "tools/fyxemu.py"),
           "--kernel", str(kernel), "--mode", "quick",
           "--keys", keys, "--keys-after", "[init] 全部通过",
           "--max-seconds", str(max_seconds), *extra]
    if test:
        cmd += ["--test", "--fb-output", str(png)]
    result = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True,
                            timeout=max_seconds + 30)
    log_path = output_dir / (name + ".log")
    log_path.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        print(result.stdout[-7000:])
        print(result.stderr[-3000:])
        raise RuntimeError(f"{name}: emulator exit {result.returncode}")
    return result.stdout + result.stderr, png


def check_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("PNG signature 错误")
    width, height = struct.unpack("!II", data[16:24])
    if (width, height) != (800, 600):
        raise RuntimeError(f"PNG 尺寸错误: {width}x{height}")
    pos = 8
    compressed = bytearray()
    while pos < len(data):
        size = struct.unpack("!I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + size]
        if kind == b"IDAT":
            compressed.extend(payload)
        pos += size + 12
    raw = zlib.decompress(bytes(compressed))
    stride = 1 + width * 3
    if len(raw) != stride * height or any(raw[row * stride] != 0
                                           for row in range(height)):
        raise RuntimeError("PNG scanline/filter 错误")
    colors = set()
    for row in range(0, height, 7):
        start = row * stride + 1
        for col in range(0, width, 11):
            p = start + col * 3
            colors.add(raw[p:p + 3])
    if len(colors) < 32:
        raise RuntimeError(f"PNG 颜色过少（{len(colors)}），不像合成桌面")
    print(f"  [PASS] PNG 800x600、非空像素、多种颜色（采样 {len(colors)} 色）")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default=str(ROOT / "build/fyxos.img"))
    parser.add_argument("--kernel", default=str(ROOT / "build/fyxos.elf"))
    parser.add_argument("--max-seconds", type=int, default=90)
    parser.add_argument("--output-dir", default=str(ROOT / "build/graphics-test"))
    args = parser.parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    failures = 0
    try:
        out, png = run_case(
            "interactive", args.kernel, output_dir, [],
            "desktop run;ntrq;ifconfig;ping;shutdown", args.max_seconds, test=True)
        if "[desktop] 交互：" not in out or "frames=4" not in out:
            raise RuntimeError("N/T/R/Q 没有产生预期的四帧")
        if "focus=NETWORK theme=light" not in out:
            raise RuntimeError("交互后焦点/主题状态错误")
        guest = re.findall(r"\[desktop\] frame=\d+ .*checksum=0x([0-9a-f]+)", out)
        host = re.findall(r"framebuffer 已保存.*checksum=0x([0-9a-f]+)", out)
        if not guest or not host or guest[-1] != host[-1]:
            raise RuntimeError("交互 PNG 与 guest 校验和不一致")
        print("  [PASS] desktop run：N/T/R/Q 与四帧重绘")
        check_png(png)
    except Exception as error:
        failures += 1
        print(f"  [FAIL] 图形交互/PNG：{error}")

    try:
        out, _ = run_case(
            "no-bga", args.kernel, output_dir, ["--no-gfx"],
            "desktop;shutdown", args.max_seconds)
        if "desktop: graphics unavailable" not in out:
            raise RuntimeError("没有返回清晰的图形设备错误")
        if "[gfx] BGA LFB" in out or "[fyxemu] 失败" in out:
            raise RuntimeError("无 BGA 路径出现意外初始化或仿真错误")
        print("  [PASS] 无 BGA 时保留救援 Shell 并正常关机")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] 无 BGA 救援路径：{error}")

    print(f"图形专项：{'通过' if failures == 0 else '失败'}（{failures} 个失败）")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
