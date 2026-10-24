#!/usr/bin/env python3
"""FYX 原生动词、Unix/CMD/PowerShell 别名与原子 rename 验收。"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"


class CliTest:
    def __init__(self, image, kernel, work, max_seconds):
        self.image = Path(image).resolve()
        self.kernel = Path(kernel).resolve()
        self.work = Path(work).resolve()
        self.max_seconds = max_seconds
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)
        self.disk = self.work / "persistent.img"
        shutil.copyfile(self.image, self.disk)

    def run(self, name, keys, *, writeback=False):
        cmd = [
            sys.executable, str(ROOT / "tools/fyxemu.py"),
            "--image", str(self.disk), "--kernel", str(self.kernel),
            "--mode", "quick", "--keys", keys, "--keys-after", READY,
            "--max-seconds", str(self.max_seconds),
        ]
        if writeback:
            cmd.append("--writeback")
        log = self.work / f"{name}.log"
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(cmd, cwd=ROOT, stdout=stream,
                                    stderr=subprocess.STDOUT,
                                    timeout=self.max_seconds + 30)
        text = log.read_text(encoding="utf-8")
        if result.returncode != 0 or "正常关机" not in text:
            raise RuntimeError(f"{name}: exit {result.returncode}\n{text[-5000:]}")
        if re.search(r"\[panic\]|kernel panic|内核 panic|CPU 异常", text, re.I):
            raise RuntimeError(f"{name}: panic/CPU 异常")
        return text

    @staticmethod
    def expect(text, needle, label):
        if needle not in text:
            raise RuntimeError(f"{label} 缺少 {needle!r}\n{text[-5000:]}")

    def run_all(self):
        root = "/home/user/cli"
        keys = (
            f"make dir {root};make file {root}/a.txt;write {root}/a.txt alpha;"
            f"make file {root}/a.txt;show file {root}/a.txt;"
            f"copy {root}/a.txt {root}/b.txt;move {root}/b.txt {root}/c.txt;"
            f"ren {root}/c.txt {root}/d.txt;move {root}/d.txt {root}/a.txt;"
            f"cat {root}/d.txt;cat {root}/a.txt;list files {root};"
            f"Get-ChildItem {root};Get-Content {root}/d.txt;Get-Process;"
            "list processes;list disks;show system;ipconfig;ver;chkdsk;explain list;"
            f"write {root}/hello.c int main(){{puts(\"CLI_BUILD_OK\")\\;return(5)\\;}};"
            f"build {root}/hello.c {root}/hello.elf;run {root}/hello.elf;shutdown"
        )
        out = self.run("commands", keys, writeback=True)
        self.expect(out, "[fs] touched", "make file/touch")
        self.expect(out, "[fs] renamed", "原子 rename")
        self.expect(out, "mv: 错误", "目标冲突拒绝覆盖")
        self.expect(out, "alpha", "touch 保留与 rename 内容")
        self.expect(out, "d.txt", "目录项")
        self.expect(out, "eth0: UP", "ipconfig")
        self.expect(out, "用法: list", "explain")
        self.expect(out, "CLI_BUILD_OK", "原生 build/ring3")
        self.expect(out, "退出码=5", "原生 build 退出码")
        print("  [PASS] 原生动词、别名、touch/冲突保护、原子 rename、build/run")

        out = self.run(
            "reboot",
            f"cat {root}/d.txt;cat {root}/a.txt;stat {root}/b.txt;"
            f"list files {root};fsck;shutdown")
        self.expect(out, "alpha", "跨重启文件内容")
        self.expect(out, "stat: 错误 -8", "旧路径已消失")
        self.expect(out, "d.txt", "跨重启新目录项")
        self.expect(out, "错误=0", "跨重启 fsck")
        print("  [PASS] rename 后新旧路径、内容和文件系统校验跨重启保持")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", default=str(ROOT / "build/fyxos.img"))
    ap.add_argument("--kernel", default=str(ROOT / "build/fyxos.elf"))
    ap.add_argument("--work", default=str(ROOT / "build/cli-test"))
    ap.add_argument("--max-seconds", type=int, default=120)
    args = ap.parse_args()
    try:
        CliTest(args.image, args.kernel, args.work, args.max_seconds).run_all()
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print(f"cli-test: FAIL: {error}", file=sys.stderr)
        return 1
    print("CLI 专项：通过（0 个失败）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
