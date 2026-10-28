#!/usr/bin/env python3
"""FYXGit v1：提交、diff、restore 与跨重启持久化验收。"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
READY = "[init] 全部通过"


class GitTest:
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
            "--mode", "quick", "--shell", "--keys", keys, "--keys-after", READY,
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
            raise RuntimeError(f"{name}: emulator exit {result.returncode}\n{text[-5000:]}")
        if re.search(r"\[panic\]|kernel panic|内核 panic|CPU 异常", text, re.I):
            raise RuntimeError(f"{name}: panic/CPU 异常")
        return text

    @staticmethod
    def expect(text, needle, label):
        if needle not in text:
            raise RuntimeError(f"{label} 缺少 {needle!r}\n{text[-5000:]}")

    def run_all(self):
        root = "/home/user/repo"
        git = f"git -C {root}"
        keys = (
            f"git init {root};write {root}/note.txt one;"
            f"{git} add note.txt;{git} status;{git} commit -m initial;"
            f"write {root}/note.txt two;{git} diff;{git} status;"
            f"{git} add note.txt;{git} diff --cached;{git} commit -m second;"
            f"{git} log;write {root}/note.txt three;{git} restore note.txt;"
            f"cat {root}/note.txt;{git} status;shutdown"
        )
        out = self.run("commands", keys, writeback=True)
        self.expect(out, "initialized empty FYXGit repository", "init")
        self.expect(out, "[git] A note.txt (staged)", "add/status")
        self.expect(out, "[git] M note.txt (worktree)", "worktree status")
        self.expect(out, "-one", "working diff old line")
        self.expect(out, "+two", "working diff new line")
        if out.count("diff --git a/note.txt b/note.txt") != 2:
            raise RuntimeError("工作区和 --cached diff 没有分别执行")
        commits = re.findall(r"\[git\] commit ([0-9a-f]{8})", out)
        if len(commits) != 2 or commits[0] == commits[1]:
            raise RuntimeError(f"提交父链/ID 错误: {commits}")
        self.expect(out, "parent=00000000 initial", "初始提交")
        self.expect(out, " second", "第二提交")
        self.expect(out, "[git] restore note.txt", "restore")
        self.expect(out, "\ntwo", "restore 内容")
        self.expect(out, "[git] clean", "restore 后 clean")
        print("  [PASS] init/add/commit/status/log/diff/restore 全链路")

        out = self.run("reboot", f"{git} log;{git} status;cat {root}/note.txt;shutdown")
        self.expect(out, "initial", "跨重启初始提交")
        self.expect(out, "second", "跨重启第二提交")
        self.expect(out, "[git] clean", "跨重启仓库状态")
        self.expect(out, "\ntwo", "跨重启文件内容")
        print("  [PASS] 仓库数据库、提交父链和恢复内容跨重启保持")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", default=str(ROOT / "build/fyxos.img"))
    ap.add_argument("--kernel", default=str(ROOT / "build/fyxos.elf"))
    ap.add_argument("--work", default=str(ROOT / "build/git-test"))
    ap.add_argument("--max-seconds", type=int, default=120)
    args = ap.parse_args()
    try:
        GitTest(args.image, args.kernel, args.work, args.max_seconds).run_all()
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print(f"git-test: FAIL: {error}", file=sys.stderr)
        return 1
    print("FYXGit 专项：通过（0 个失败）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
