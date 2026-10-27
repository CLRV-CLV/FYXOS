#!/usr/bin/env python3
"""Accept the static Linux x86_64 + uncompressed amd64 DEB compatibility slice."""
from __future__ import annotations

import argparse
import io
import re
import struct
import subprocess
import sys
import tarfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"
EXPECTED_OUTPUT = "LINUX_GLIBC_OK\nDEB_PACKAGE_OK\n"


def ar_members(package: bytes) -> dict[str, bytes]:
    if not package.startswith(b"!<arch>\n"):
        raise RuntimeError("not a Debian ar archive")
    members: dict[str, bytes] = {}
    offset = 8
    while offset < len(package):
        if offset + 60 > len(package):
            raise RuntimeError("truncated ar header")
        header = package[offset:offset + 60]
        if header[58:60] != b"`\n":
            raise RuntimeError("bad ar header trailer")
        name = header[:16].decode("ascii").rstrip().rstrip("/")
        size = int(header[48:58].decode("ascii").strip())
        offset += 60
        if size > len(package) - offset:
            raise RuntimeError("truncated ar member")
        if name in members:
            raise RuntimeError(f"duplicate ar member {name}")
        members[name] = package[offset:offset + size]
        offset += size + (size & 1)
    return members


def inspect_package(package: Path, installed_elf: Path) -> tuple[int, int]:
    members = ar_members(package.read_bytes())
    if set(members) != {"debian-binary", "control.tar", "data.tar"}:
        raise RuntimeError(f"unexpected DEB members: {sorted(members)}")
    if members["debian-binary"] != b"2.0\n":
        raise RuntimeError("unsupported debian-binary version")
    with tarfile.open(fileobj=io.BytesIO(members["control.tar"]), mode="r:") as tar:
        control_member = tar.getmember("./control")
        control = tar.extractfile(control_member).read().decode("utf-8")
    fields = {}
    for line in control.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            fields[key] = value.strip()
    if fields.get("Architecture") != "amd64" or fields.get("Depends"):
        raise RuntimeError("test package must be amd64 with no dependencies")

    with tarfile.open(fileobj=io.BytesIO(members["data.tar"]), mode="r:") as tar:
        payload_member = tar.getmember("./usr/bin/fyx-deb-demo")
        payload = tar.extractfile(payload_member).read()
    reference = installed_elf.read_bytes()
    if payload != reference:
        raise RuntimeError("DEB payload differs from the GCC output")
    if len(payload) < 64 or payload[:4] != b"\x7fELF":
        raise RuntimeError("payload is not ELF")
    if payload[4] != 2 or payload[5] != 1 or payload[7] != 3:
        raise RuntimeError("payload is not ELF64 little-endian GNU OSABI")
    kind, machine = struct.unpack_from("<HH", payload, 16)
    if kind != 2 or machine != 62:
        raise RuntimeError("payload is not x86_64 ET_EXEC")
    phoff = struct.unpack_from("<Q", payload, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", payload, 54)
    for index in range(phnum):
        p_type = struct.unpack_from("<I", payload, phoff + index * phentsize)[0]
        if p_type == 3:
            raise RuntimeError("payload unexpectedly requires PT_INTERP")
    return len(payload), zlib.crc32(payload) & 0xFFFFFFFF


def run_guest(kernel: Path, output: Path) -> str:
    keys = ";".join([
        "deb /packages/fyx-deb-demo.deb",
        "stat /usr/bin/fyx-deb-demo",
        "run linux /usr/bin/fyx-deb-demo",
        "run /bin/gcc-c11-demo",
        "deb /bin/init",  # 非 ar：必须拒绝且不能破坏已安装文件
        "fsck",
        "shutdown",
    ])
    command = [
        sys.executable, ROOT / "tools" / "fyxemu.py", "--kernel", kernel,
        "--mode", "quick", "--no-net", "--no-gfx", "--keys", keys,
        "--keys-after", READY, "--max-seconds", "120",
    ]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                            timeout=150)
    text = result.stdout + result.stderr
    output.write_text(text)
    if result.returncode:
        raise RuntimeError(f"emulator returned {result.returncode}\n{text[-6000:]}")
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fyxos.elf")
    parser.add_argument("--package", type=Path,
                        default=ROOT / "build/compat/fyx-deb-demo.deb")
    parser.add_argument("--payload", type=Path,
                        default=ROOT / "build/compat/deb-demo/root/usr/bin/fyx-deb-demo")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/compat-test")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    failures = 0

    def check(name: str, condition: bool) -> None:
        nonlocal failures
        print(f"  [{'PASS' if condition else 'FAIL'}] {name}")
        failures += not condition

    try:
        payload_size, payload_crc = inspect_package(args.package, args.payload)
        check("标准 DEB 三成员、amd64 control 与包内 ELF 逐字节一致", True)
    except Exception as error:
        print(f"  [FAIL] 标准 DEB/ELF 宿主检查：{error}")
        return 1

    reference = subprocess.run([args.payload], text=True, capture_output=True,
                               timeout=15)
    check("宿主参考程序输出两行并以 23 退出",
          reference.returncode == 23 and reference.stdout == EXPECTED_OUTPUT and
          reference.stderr == "")

    try:
        text = run_guest(args.kernel.resolve(), args.output_dir / "guest.log")
    except Exception as error:
        print(f"  [FAIL] guest 兼容纵切片：{error}")
        return 1

    install_ok = bool(re.search(
        r"\[deb\] 已安装 fyx-deb-demo 1\.0-1：1 个文件，\d+ 字节", text))
    stat_pattern = (rf"/usr/bin/fyx-deb-demo: node=\d+ type=file "
                    rf"size={payload_size} crc32=0x{payload_crc:08x}")
    check("guest 解包安装且尺寸/CRC32 与宿主载荷一致",
          install_ok and re.search(stat_pattern, text) is not None)
    check("guest 运行包内未修改 glibc 程序并输出两行",
          "LINUX_GLIBC_OK" in text and "DEB_PACKAGE_OK" in text)
    check("guest Linux 程序真实退出码为 23",
          "[run] /usr/bin/fyx-deb-demo 退出码=23" in text)
    check("Linux 运行后 FYX 原生 ABI 仍可执行",
          "GCC_C11_OK" in text and "[run] /bin/gcc-c11-demo 退出码=42" in text)
    check("非 DEB 输入明确拒绝，安装后 FYXFS 校验通过",
          "deb: 安装失败：/bin/init（错误 -1）" in text and
          "[fsck] 校验通过" in text and "错误=0" in text)
    check("正常关机且无 panic、CPU 异常或非法指令",
          "正常关机" in text and not re.search(
              r"\[panic\]|\*\*\* EXCEPTION|CPU 异常|Invalid instruction", text,
              re.IGNORECASE))
    unknown = {int(value) for value in re.findall(r"\[linux\] ENOSYS .* nr=(\d+)", text)}
    check("syscall 无隐藏缺口：只允许 rseq/getrandom 回退，单调时钟已实现",
          unknown <= {318, 334} and 228 not in unknown and
          "[linux] 未识别调用累计=" in text)

    print(f"  兼容套件汇总：{9 - failures} PASS / {failures} FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
