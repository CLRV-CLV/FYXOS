#!/usr/bin/env python3
"""DNS/UDP/TCP/ICMP 端到端验收：真实 guest 协议栈与异常响应路径。"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"


def expect(text: str, needle: str, label: str):
    if needle not in text:
        raise RuntimeError(f"{label} 缺少 {needle!r}\n{text[-5000:]}")


def expect_resolve_error(text: str, name: str, code: int, label: str):
    # 串口台架偶尔会损坏一个 UTF-8 字符；主机名与错误码仍须在同一输出行。
    pattern = rf"resolve: {re.escape(name)}[^\r\n]{{0,48}}错误 {code}"
    if not re.search(pattern, text):
        raise RuntimeError(f"{label} 缺少 {name} / {code}\n{text[-5000:]}")


def run_dns_case(kernel: str, output_dir: Path, max_seconds: int) -> str:
    output_dir.mkdir(parents=True, exist_ok=True)
    keys = (
        "ifconfig;resolve example.test;resolve EXAMPLE.TEST.;"
        "resolve tcpfallback.test;resolve retry.test 80;"
        "resolve RETRY.TEST. 20;resolve expire.test;sleep 1100;"
        "resolve expire.test;resolve missing.test;"
        "resolve empty.test;resolve broken.test;resolve bad..name;"
        "resolve unknown.test 20;ping gateway.test;shutdown"
    )
    cmd = [
        sys.executable, str(ROOT / "tools/fyxemu.py"),
        "--kernel", kernel, "--mode", "quick", "--keys", keys,
        "--keys-after", READY, "--test", "--max-seconds", str(max_seconds),
    ]
    result = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True,
                            timeout=max_seconds + 30)
    text = result.stdout + result.stderr
    (output_dir / "dns.log").write_text(text, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"emulator exit {result.returncode}\n{text[-6000:]}")
    if "[fyxemu] 测试通过" not in text or "正常关机" not in text:
        raise RuntimeError("基础自检或正常关机失败")
    if re.search(r"\[panic\]|kernel panic|内核 panic|CPU 异常", text,
                 re.IGNORECASE):
        raise RuntimeError("出现 panic/CPU 异常")
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kernel", default=str(ROOT / "build/fyxos.elf"))
    ap.add_argument("--output-dir", default=str(ROOT / "build/network-test"))
    ap.add_argument("--max-seconds", type=int, default=90)
    args = ap.parse_args()
    try:
        out = run_dns_case(args.kernel, Path(args.output_dir), args.max_seconds)
    except Exception as error:
        print(f"  [FAIL] DNS 启动/收发：{error}")
        print("DNS/网络专项：失败（1 个失败）")
        return 1

    failures = 0
    try:
        expect(out, "DNS  10.0.2.2", "DHCP option 6")
        expect(out, "[dns] example.test -> 93.184.216.34", "压缩 A 记录")
        expect(out, "[dns] EXAMPLE.TEST. -> 93.184.216.34", "大小写/根点/连续查询")
        if "203.0.113.99" in out:
            raise RuntimeError("错误事务 ID 的地址被接受")
        print("  [PASS] DHCP DNS、真实 UDP socket、错误 ID 过滤与压缩 A 记录")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] DNS 正向解析：{error}")

    try:
        expect(out, "[dns] tcpfallback.test -> 93.184.216.35",
               "UDP TC 后的 DNS-over-TCP 回退")
        print("  [PASS] DNS UDP 截断 → TCP 握手 → 长度前缀查询与 A 记录")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] DNS TCP 回退：{error}")

    try:
        expect(out, "[dns] retry.test -> 93.184.216.36",
               "首查询丢失后的同 ID UDP 重传")
        expect(out, "[dns] RETRY.TEST. -> 93.184.216.36",
               "规范名 TTL 缓存命中")
        expect(out, "[dns] expire.test -> 93.184.216.37", "TTL 到期前的地址")
        expect(out, "[dns] expire.test -> 93.184.216.38",
               "TTL 到期后的重新查询与缓存更新")
        print("  [PASS] DNS UDP 重传、规范名 TTL 缓存与到期重新查询")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] DNS 重传/缓存：{error}")

    try:
        if not re.search(r"UDP[^\r\n]*=1/1/1", out):
            raise RuntimeError("坏 UDP checksum 未计入丢弃统计")
        if not re.search(r"TCP[^\r\n]*=4/7/1", out):
            raise RuntimeError("TCP 数据重传/坏 SYN-ACK 统计不符")
        print("  [PASS] TCP SYN/数据重传与 UDP checksum 生成、坏帧丢弃")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] TCP/UDP 可靠性：{error}")

    try:
        expect_resolve_error(out, "missing.test", -8, "NXDOMAIN")
        expect_resolve_error(out, "empty.test", -8, "无 A 记录")
        expect_resolve_error(out, "broken.test", -21, "压缩指针环路")
        expect_resolve_error(out, "bad..name", -1, "非法主机名")
        expect_resolve_error(out, "unknown.test", -18, "查询超时")
        print("  [PASS] NXDOMAIN、空答案、指针环路、非法名与超时诊断")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] DNS 负向解析：{error}")

    try:
        expect(out, "ping: gateway.test -> 10.0.2.2", "ping 主机名解析")
        expect(out, "ping 10.0.2.2: ICMP echo 回复", "解析后 ICMP 往返")
        print("  [PASS] ping 主机名 → DNS A 查询 → ICMP 端到端")
    except Exception as error:
        failures += 1
        print(f"  [FAIL] DNS/ICMP 组合：{error}")

    print(f"DNS/网络专项：{'通过' if failures == 0 else '失败'}（{failures} 个失败）")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
