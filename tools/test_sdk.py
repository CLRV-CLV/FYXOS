#!/usr/bin/env python3
"""Verify the open-source GCC SDK on host and its preinstalled ELF in ring3."""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"


def check_elf(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise RuntimeError(f"{path}: not a little-endian ELF64 file")
    (kind, machine, _version, entry, phoff, _shoff, _flags, _ehsize,
     phentsize, phnum, _shentsize, _shnum, _shstr) = struct.unpack_from(
        "<HHIQQQIHHHHHH", data, 16)
    if kind != 2 or machine != 62 or entry != 0x400000:
        raise RuntimeError(f"{path}: wrong type/machine/entry")
    loads = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        p_type, p_flags, _off, vaddr, _phys, filesz, memsz, _align = \
            struct.unpack_from("<IIQQQQQQ", data, offset)
        if p_type == 1:
            if p_flags & 1 and p_flags & 2:
                raise RuntimeError(f"{path}: writable and executable LOAD")
            loads.append((p_flags, vaddr, filesz, memsz))
    if not loads or not any(flags & 1 for flags, *_rest in loads):
        raise RuntimeError(f"{path}: no executable LOAD")
    if not any(flags & 1 and vaddr <= entry < vaddr + memsz
               for flags, vaddr, _filesz, memsz in loads):
        raise RuntimeError(f"{path}: entry is not in an executable segment")


def compile_examples(sdk: Path, output: Path) -> None:
    compiler = sdk / "bin" / "fyx-gcc"
    version = subprocess.run([compiler, "--version"], cwd=output, text=True,
                             capture_output=True, check=True)
    if "gcc" not in (version.stdout + version.stderr).lower():
        raise RuntimeError("SDK version output does not identify GCC")
    for name in ("c11-demo", "hello"):
        result = subprocess.run(
            [compiler, sdk / "examples" / f"{name}.c", "-o", output / f"{name}.elf"],
            cwd=output, text=True, capture_output=True)
        log = result.stdout + result.stderr
        (output / f"{name}-compile.log").write_text(log)
        if result.returncode or re.search(r"warning:|error:", log, re.IGNORECASE):
            raise RuntimeError(f"{name}: host SDK compilation failed\n{log}")
        check_elf(output / f"{name}.elf")


def run_guest(kernel: Path, output: Path) -> None:
    command = [
        sys.executable, ROOT / "tools" / "fyxemu.py", "--kernel", kernel,
        "--mode", "quick", "--keys",
        "ifconfig;ping;run /bin/gcc-c11-demo;shutdown",
        "--keys-after", READY, "--test", "--max-seconds", "90",
    ]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                            timeout=120)
    text = result.stdout + result.stderr
    (output / "guest.log").write_text(text)
    if result.returncode or "[fyxemu] 测试通过" not in text or "正常关机" not in text:
        raise RuntimeError(f"guest SDK demo failed\n{text[-6000:]}")
    for needle in ("GCC_C11_OK", "[run] /bin/gcc-c11-demo 退出码=42"):
        if needle not in text:
            raise RuntimeError(f"guest output missing {needle!r}\n{text[-4000:]}")
    if re.search(r"\[panic\]|kernel panic|CPU 异常", text, re.IGNORECASE):
        raise RuntimeError("guest SDK demo raised a kernel/CPU exception")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fyxos.elf")
    parser.add_argument("--sdk-dir", type=Path, default=ROOT / "build/fyxos-sdk")
    parser.add_argument("--demo", type=Path, default=ROOT / "build/sdk/c11-demo.elf")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/sdk-test")
    args = parser.parse_args()
    args.kernel = args.kernel.resolve()
    args.demo = args.demo.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    try:
        check_elf(args.demo)
        compile_examples(args.sdk_dir.resolve(), args.output_dir)
        run_guest(args.kernel, args.output_dir)
    except Exception as error:
        print(f"  [FAIL] 开源 GCC C11 SDK：{error}")
        return 1
    print("  [PASS] 开源 GCC C11 SDK：独立编译、ELF64 W^X 与预装 ring3 运行")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
