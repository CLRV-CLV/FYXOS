#!/usr/bin/env python3
"""Accept gzip/plain DEB and Win64 PE32+ compatibility with real guest execution."""
from __future__ import annotations

import argparse
import binascii
import gzip
import io
import os
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


def tar_bytes(members: dict[str, bytes], stem: str) -> bytes:
    plain, compressed = stem + ".tar", stem + ".tar.gz"
    if (plain in members) == (compressed in members):
        raise RuntimeError(f"expected exactly one {plain}[.gz]")
    return gzip.decompress(members[compressed]) if compressed in members else members[plain]


def one_tar_file(data: bytes, name: str) -> bytes:
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as tar:
        extracted = tar.extractfile(tar.getmember(name))
        if extracted is None:
            raise RuntimeError(f"{name} is not a regular file")
        return extracted.read()


def control_fields(data: bytes) -> dict[str, str]:
    text = one_tar_file(data, "./control").decode("utf-8")
    fields = {}
    for line in text.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            fields[key] = value.strip()
    if fields.get("Architecture") != "amd64" or fields.get("Depends"):
        raise RuntimeError("test package must be amd64 with no dependencies")
    return fields


def inspect_gzip_package(package: Path, installed_elf: Path) -> tuple[int, int]:
    members = ar_members(package.read_bytes())
    if set(members) != {"debian-binary", "control.tar.gz", "data.tar.gz"}:
        raise RuntimeError(f"unexpected gzip DEB members: {sorted(members)}")
    if members["debian-binary"] != b"2.0\n":
        raise RuntimeError("unsupported debian-binary version")
    control_fields(tar_bytes(members, "control"))
    payload = one_tar_file(tar_bytes(members, "data"), "./usr/bin/fyx-deb-demo")
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
    return len(payload), binascii.crc32(payload) & 0xFFFFFFFF


def inspect_plain_package(package: Path) -> tuple[int, int]:
    members = ar_members(package.read_bytes())
    if set(members) != {"debian-binary", "control.tar", "data.tar"}:
        raise RuntimeError("plain DEB does not contain uncompressed tar members")
    control_fields(tar_bytes(members, "control"))
    note = one_tar_file(tar_bytes(members, "data"), "./etc/fyx-deb-plain-ok")
    if note != b"UNCOMPRESSED_DEB_OK\n":
        raise RuntimeError("plain DEB note differs")
    return len(note), binascii.crc32(note) & 0xFFFFFFFF


def inspect_bad_package(package: Path) -> None:
    members = ar_members(package.read_bytes())
    control_fields(tar_bytes(members, "control"))
    try:
        gzip.decompress(members["data.tar.gz"])
    except (gzip.BadGzipFile, EOFError, zlib.error):
        return
    raise RuntimeError("negative DEB unexpectedly has valid gzip data")


def inspect_pe(path: Path, expected_names: list[str]) -> None:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise RuntimeError("not MZ")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise RuntimeError("not PE")
    machine, sections, _, _, _, opt_size = struct.unpack_from("<HHIIIH", data, pe + 4)
    opt = pe + 24
    if machine != 0x8664 or struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise RuntimeError("not AMD64 PE32+")
    if struct.unpack_from("<Q", data, opt + 24)[0] != 0x400000:
        raise RuntimeError("unexpected ImageBase")
    entry = struct.unpack_from("<I", data, opt + 16)[0]
    if struct.unpack_from("<I", data, opt + 108)[0] < 2:
        raise RuntimeError("missing import directory")
    import_rva = struct.unpack_from("<I", data, opt + 120)[0]
    table = []
    sh = opt + opt_size
    for index in range(sections):
        off = sh + index * 40
        vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        chars = struct.unpack_from("<I", data, off + 36)[0]
        table.append((vsize, rva, raw_size, raw_ptr, chars))
        if rva <= entry < rva + vsize and not chars & 0x20000000:
            raise RuntimeError("entry is not executable")

    def at(rva: int, size: int = 1) -> bytes:
        for vsize, base, raw_size, raw_ptr, _ in table:
            delta = rva - base
            if 0 <= delta and delta + size <= min(vsize, raw_size):
                return data[raw_ptr + delta:raw_ptr + delta + size]
        raise RuntimeError(f"RVA {rva:#x} outside file-backed section")

    def cstr(rva: int) -> str:
        raw = bytearray()
        for index in range(64):
            char = at(rva + index)[0]
            if not char:
                return raw.decode("ascii")
            raw.append(char)
        raise RuntimeError("unterminated import string")

    desc = at(import_rva, 20)
    lookup, _, _, dll_rva, iat = struct.unpack("<IIIII", desc)
    if cstr(dll_rva).lower() != "kernel32.dll":
        raise RuntimeError("unexpected DLL")
    names = []
    for index in range(16):
        thunk = struct.unpack("<Q", at((lookup or iat) + index * 8, 8))[0]
        if not thunk:
            break
        if thunk >> 63:
            raise RuntimeError("unexpected ordinal")
        names.append(cstr((thunk & 0x7FFFFFFF) + 2))
    if names != expected_names:
        raise RuntimeError(f"unexpected imports: {names}")


def run_gzip_host_tests(work: Path) -> None:
    work.mkdir(parents=True, exist_ok=True)
    driver = work / "driver"
    compile_result = subprocess.run([
        "gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", ROOT / "include", ROOT / "tools/gzip_test_driver.c",
        ROOT / "kernel/lib/gzip.c", "-o", driver,
    ], text=True, capture_output=True)
    if compile_result.returncode:
        raise RuntimeError(compile_result.stdout + compile_result.stderr)
    raw = (b"FYXOS gzip/DEFLATE integration\n" * 2000) + bytes(range(256)) * 200
    (work / "raw").write_bytes(raw)
    positive = {}
    for level in (0, 1, 9):
        positive[f"level-{level}"] = gzip.compress(raw, compresslevel=level, mtime=0)
    compressor = zlib.compressobj(6, zlib.DEFLATED, -15, zlib.DEF_MEM_LEVEL, zlib.Z_FIXED)
    fixed = compressor.compress(raw) + compressor.flush()
    header = b"\x1f\x8b\x08\x1e" + bytes(6)
    header += struct.pack("<H", 3) + b"xyz" + b"fixture-name\0" + b"comment\0"
    header += struct.pack("<H", binascii.crc32(header) & 0xFFFF)
    positive["fixed+optional-header"] = header + fixed + struct.pack(
        "<II", binascii.crc32(raw) & 0xFFFFFFFF, len(raw))
    negative = {
        "bad-crc": positive["level-9"][:-8] + bytes([positive["level-9"][-8] ^ 1]) + positive["level-9"][-7:],
        "bad-isize": positive["level-9"][:-1] + bytes([positive["level-9"][-1] ^ 1]),
        "trailing": positive["level-9"] + b"junk",
        "truncated": positive["level-9"][:-9],
        "multi-member": positive["level-9"] + positive["level-9"],
    }
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1")
    for name, content in positive.items():
        case = work / f"{name}.gz"
        case.write_bytes(content)
        result = subprocess.run([driver, case, work / "raw", str(len(raw))],
                                text=True, capture_output=True, env=env)
        if result.returncode or "Sanitizer" in result.stderr or "runtime error" in result.stderr:
            raise RuntimeError(f"{name}: {result.stdout}{result.stderr}")
    for name, content in negative.items():
        case = work / f"{name}.gz"
        case.write_bytes(content)
        result = subprocess.run([driver, case, work / "raw", str(len(raw))],
                                text=True, capture_output=True, env=env)
        if result.returncode != 1 or "gzip mismatch:" not in result.stderr or \
                "Sanitizer" in result.stderr or "runtime error" in result.stderr:
            raise RuntimeError(f"{name} was not cleanly rejected: {result.stdout}{result.stderr}")
    case = work / "limit.gz"
    case.write_bytes(positive["level-9"])
    result = subprocess.run([driver, case, work / "raw", str(len(raw) - 1)],
                            text=True, capture_output=True, env=env)
    if result.returncode != 1 or "gzip mismatch:" not in result.stderr:
        raise RuntimeError("output limit was not enforced")


def run_guest(kernel: Path, image: Path, output: Path, mode: str = "quick",
              keys: str | None = None) -> str:
    if keys is None:
        keys = ";".join([
        "deb /packages/fyx-deb-bad-gzip.deb",
        "stat /etc/fyx-deb-plain-ok",
        "deb /packages/fyx-deb-plain-demo.deb",
        "cat /etc/fyx-deb-plain-ok",
        "cp /etc/fyx-deb-plain-ok /tmp/fyx-deb-plain-copy",
        "stat /etc/fyx-deb-plain-ok",
        "stat /tmp/fyx-deb-plain-copy",
        "deb /packages/fyx-deb-demo.deb",
        "stat /usr/bin/fyx-deb-demo",
        "run linux /usr/bin/fyx-deb-demo",
        "run win64 /bin/win64-bad-import.exe",
        "run win64 /bin/win64-demo.exe",
        "run win64 /bin/init",
        "run linux /bin/win64-demo.exe",
        "run /bin/gcc-c11-demo",
        "deb /bin/init",
        "fsck",
        "shutdown",
    ])
    command = [
        sys.executable, ROOT / "tools/fyxemu.py", "--kernel", kernel,
        "--image", image, "--mode", mode, "--no-net", "--no-gfx", "--keys", keys,
        "--keys-after", READY, "--max-seconds", "150",
    ]
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                            timeout=180)
    text = result.stdout + result.stderr
    output.write_text(text)
    if result.returncode:
        raise RuntimeError(f"emulator returned {result.returncode}\n{text[-6000:]}")
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fyxos.elf")
    parser.add_argument("--image", type=Path, default=ROOT / "build/fyxos.img")
    parser.add_argument("--package", type=Path, default=ROOT / "build/compat/fyx-deb-demo.deb")
    parser.add_argument("--plain-package", type=Path, default=ROOT / "build/compat/fyx-deb-plain-demo.deb")
    parser.add_argument("--bad-package", type=Path, default=ROOT / "build/compat/fyx-deb-bad-gzip.deb")
    parser.add_argument("--win64", type=Path, default=ROOT / "build/compat/win64-demo.exe")
    parser.add_argument("--bad-win64", type=Path, default=ROOT / "build/compat/win64-bad-import.exe")
    parser.add_argument("--payload", type=Path, default=ROOT / "build/compat/deb-demo/root/usr/bin/fyx-deb-demo")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/compat-test")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    failures = checks = 0

    def check(name: str, condition: bool) -> None:
        nonlocal failures, checks
        checks += 1
        print(f"  [{'PASS' if condition else 'FAIL'}] {name}")
        failures += not condition

    try:
        payload_size, payload_crc = inspect_gzip_package(args.package, args.payload)
        check("gzip DEB 三成员、control 与包内未修改 ELF 逐字节一致", True)
        plain_size, plain_crc = inspect_plain_package(args.plain_package)
        check("未压缩 DEB 保留原始 control.tar/data.tar 与目标内容", True)
        inspect_bad_package(args.bad_package)
        check("负向 DEB 确实是 control 合法但 data CRC32 损坏的 gzip 包", True)
        inspect_pe(args.win64, ["GetStdHandle", "WriteFile", "ExitProcess"])
        check("Win64 载荷为真实 PE32+ 且 kernel32 三项导入完整", True)
        inspect_pe(args.bad_win64, ["GetStdHandle", "XriteFile", "ExitProcess"])
        check("未知导入负向 PE 保持结构合法且仅 API 名称改变", True)
        run_gzip_host_tests(args.output_dir / "host-gzip")
        check("同一内核 gzip C 源码通过 stored/fixed/dynamic 与 6 组负例", True)
    except Exception as error:
        print(f"  [FAIL] 宿主兼容载荷/解码器检查：{error}")
        return 1

    reference = subprocess.run([args.payload], text=True, capture_output=True, timeout=15)
    check("宿主参考程序输出两行并以 23 退出",
          reference.returncode == 23 and reference.stdout == EXPECTED_OUTPUT and not reference.stderr)
    try:
        text = run_guest(args.kernel.resolve(), args.image.resolve(),
                         args.output_dir / "guest.log")
    except Exception as error:
        print(f"  [FAIL] guest 兼容纵切片：{error}")
        return 1

    bad_end = text.find("deb /packages/fyx-deb-plain-demo.deb")
    bad_segment = text[text.find("deb /packages/fyx-deb-bad-gzip.deb"):bad_end]
    check("损坏 gzip CRC32 返回 EIO 且目标文件在安装前不存在",
          "deb: 安装失败：/packages/fyx-deb-bad-gzip.deb（错误 -7）" in bad_segment and
          "stat: 错误 -8" in bad_segment)
    # 控制台环形日志偶尔会出现 NUL/坏 UTF-8；用 guest 实际读回并复制后的
    # 两份尺寸+CRC32 验证内容，不把单次终端打印损伤误判为 DEB 数据错误。
    plain_stat = rf"/etc/fyx-deb-plain-ok: node=\d+ type=file size={plain_size} crc32=0x{plain_crc:08x}"
    copy_stat = rf"/tmp/fyx-deb-plain-copy: node=\d+ type=file size={plain_size} crc32=0x{plain_crc:08x}"
    check("guest 未压缩 DEB 安装并读出原内容",
          "INSTALL_OK package=fyx-deb-plain-demo version=1.0-1 files=1 bytes=20 arch=amd64 format=plain" in text and
          re.search(plain_stat, text) is not None and
          re.search(copy_stat, text) is not None)
    install_ok = (f"INSTALL_OK package=fyx-deb-demo version=1.0-1 files=1 "
                  f"bytes={payload_size} arch=amd64 format=gzip") in text
    stat_pattern = rf"/usr/bin/fyx-deb-demo: node=\d+ type=file size={payload_size} crc32=0x{payload_crc:08x}"
    check("guest gzip DEB 解压安装且尺寸/CRC32 与宿主载荷一致",
          install_ok and re.search(stat_pattern, text) is not None)
    check("guest 运行 gzip 包内未修改 glibc 程序并输出两行",
          "LINUX_GLIBC_OK" in text and "DEB_PACKAGE_OK" in text)
    check("guest Linux 程序真实退出码为 23", "[run] /usr/bin/fyx-deb-demo 退出码=23" in text)
    check("未知 kernel32 导入明确拒绝且可继续调度",
          "不支持的 kernel32 导入：XriteFile" in text and
          "run: 无法启动 /bin/win64-bad-import.exe（错误 -3）" in text)
    check("Win64 经真实 IAT 调用 WriteFile 并输出两项标记",
          "WIN64_PE_OK" in text and "WIN32_IMPORT_OK" in text)
    check("ExitProcess(24) 形成真实退出码且没有隐藏 Win32 缺口",
          "[run] /bin/win64-demo.exe 退出码=24" in text and "[win32] 未实现服务累计=0" in text)
    check("ELF/PE 跨格式输入均被对应装载器拒绝",
          "run: 无法启动 /bin/init（错误 -1）" in text and
          "run: 无法启动 /bin/win64-demo.exe（错误 -1）" in text)
    check("Linux/Win64 运行后 FYX 原生 ABI 仍可执行",
          "GCC_C11_OK" in text and "[run] /bin/gcc-c11-demo 退出码=42" in text)
    check("非 DEB 输入明确拒绝，全部安装后 FYXFS 校验通过",
          "deb: 安装失败：/bin/init（错误 -1）" in text and "[fsck] 校验通过" in text and "错误=0" in text)
    check("正常关机且无 panic、CPU 异常或非法指令",
          "正常关机" in text and not re.search(r"\[panic\]|\*\*\* EXCEPTION|CPU 异常|Invalid instruction", text, re.IGNORECASE))
    unknown = {int(value) for value in re.findall(r"\[linux\] ENOSYS .* nr=(\d+)", text)}
    check("Linux 无隐藏缺口：只允许 rseq/getrandom 回退且单调时钟已实现",
          unknown <= {318, 334} and 228 not in unknown and "[linux] 未识别调用累计=" in text)

    full_text = ""
    try:
        full_keys = ";".join([
            "stat /packages/fyx-deb-demo.deb",
            "deb /packages/fyx-deb-demo.deb",
            "run linux /usr/bin/fyx-deb-demo",
            "run win64 /bin/win64-demo.exe",
            "fsck",
            "shutdown",
        ])
        full_text = run_guest(args.kernel.resolve(), args.image.resolve(),
                              args.output_dir / "full-boot.log",
                              mode="full-boot", keys=full_keys)
    except Exception:
        pass                         # full-boot.log 保留具体模拟器/内核错误
    package_crc = binascii.crc32(args.package.read_bytes()) & 0xFFFFFFFF
    full_stat = (rf"/packages/fyx-deb-demo.deb: node=\d+ type=file "
                 rf"size={args.package.stat().st_size} crc32=0x{package_crc:08x}")
    full_install = (f"INSTALL_OK package=fyx-deb-demo version=1.0-1 files=1 "
                    f"bytes={payload_size} arch=amd64 format=gzip")
    check("完整 BIOS/stage2 保持载荷 CRC 并运行 gzip Linux 与 Win64",
          re.search(full_stat, full_text) is not None and
          full_install in full_text and "LINUX_GLIBC_OK" in full_text and
          "[run] /usr/bin/fyx-deb-demo 退出码=23" in full_text and
          "WIN32_IMPORT_OK" in full_text and
          "[run] /bin/win64-demo.exe 退出码=24" in full_text and
          "正常关机" in full_text and
          not re.search(r"\[panic\]|\*\*\* EXCEPTION|CPU 异常", full_text,
                        re.IGNORECASE))

    print(f"  兼容套件汇总：{checks - failures} PASS / {failures} FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
