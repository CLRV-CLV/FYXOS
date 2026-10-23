#!/usr/bin/env python3
"""FYXFS 持久化、快照/回滚与 ATA 写点断电集成测试。

必须由 make（或手工）先构建 --image/--kernel。所有状态镜像放在 build/ 下，
不会修改原始可引导镜像。断电用例并行运行，每个用例都有独立 ATA 盘。
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import sys

READY = "[init] 全部通过"
# 用目标文件名而不是整行日志武装：键盘回显完成后、该命令实际 ATA 写前
# 即会命中；即使台架偶发丢掉其他日志字节，也不会漏掉断电窗口。
WRITE_MARKER = "atomic.txt"


class StorageTest:
    def __init__(self, image, kernel, work, max_seconds):
        self.image = Path(image).resolve()
        self.kernel = Path(kernel).resolve()
        self.work = Path(work).resolve()
        self.max_seconds = max_seconds
        self.python = sys.executable
        self.emu = Path(__file__).with_name("fyxemu.py")
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)

    def run(self, name, disk, keys, *, writeback=False, power_write=None):
        disk = Path(disk)
        log_path = self.work / f"{name}.log"
        cmd = [
            self.python, str(self.emu),
            "--image", str(disk), "--kernel", str(self.kernel),
            "--mode", "quick", "--keys", keys, "--keys-after", READY,
            "--max-seconds", str(self.max_seconds),
        ]
        if writeback:
            cmd.append("--writeback")
        if power_write is not None:
            cmd += ["--power-fail-after", WRITE_MARKER,
                    "--power-fail-write", str(power_write)]
        with log_path.open("wb") as log:
            result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT,
                                    cwd=self.image.parent.parent, timeout=self.max_seconds + 20)
        text = log_path.read_bytes().decode("utf-8", "replace")
        if result.returncode != 0:
            raise RuntimeError(f"{name} 退出码 {result.returncode}\n{text[-5000:]}")
        if power_write is not None:
            self.expect(text, "模拟断电", name)
            self.expect(text, "ATA 介质已保存", name)
        else:
            self.expect(text, "正常关机", name)
        return text

    @staticmethod
    def expect(text, needle, name):
        if needle not in text:
            raise RuntimeError(f"{name} 缺少 {needle!r}\n{text[-5000:]}")

    def prepare_atomic(self):
        disk = self.work / "atomic-base.img"
        shutil.copyfile(self.image, disk)
        out = self.run("atomic-prepare", disk,
                       "write /etc/atomic.txt original;shutdown", writeback=True)
        self.expect(out, "已原子写入", "atomic-prepare")
        return disk

    def crash_case(self, base, cut):
        expected = "changed" if cut >= 17 else "original"
        disk = self.work / f"crash-{cut}.img"
        shutil.copyfile(base, disk)
        self.run(f"crash-{cut}", disk, "write /etc/atomic.txt changed",
                 writeback=True, power_write=cut)
        out = self.run(f"crash-{cut}-verify", disk,
                       "cat /etc/atomic.txt;fsck;shutdown")
        self.expect(out, f"\n{expected}", f"crash-{cut}-verify content")
        self.expect(out, "错误=0", f"crash-{cut}-verify fsck")
        if "panic" in out.lower():
            raise RuntimeError(f"crash-{cut}-verify 出现 panic")
        return cut, expected

    def snapshot_case(self):
        disk = self.work / "persistent.img"
        shutil.copyfile(self.image, disk)
        out = self.run(
            "snapshot-rollback", disk,
            "write /etc/system.txt before;"
            "write /home/user/keep.txt original;"
            "snapshot create clean;"
            "write /etc/system.txt after;"
            "write /home/user/keep.txt personal;"
            "rollback 1;cat /etc/system.txt;cat /home/user/keep.txt;"
            "unmount;mount;fsck;shutdown",
            writeback=True)
        self.expect(out, "\nbefore", "系统文件回滚")
        self.expect(out, "\npersonal", "/home 保留")
        self.expect(out, "错误=0", "回滚后 fsck")

        out = self.run(
            "persistent-reboot", disk,
            "cat /etc/system.txt;cat /home/user/keep.txt;snapshot;fsck;shutdown",
            writeback=True)
        self.expect(out, "\nbefore", "跨重启系统文件")
        self.expect(out, "\npersonal", "跨重启个人文件")
        self.expect(out, "clean", "跨重启快照元数据")
        self.expect(out, "错误=0", "跨重启 fsck")

    def run_all(self):
        base = self.prepare_atomic()
        self.snapshot_case()
        cuts = (1, 9, 16, 17)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = [pool.submit(self.crash_case, base, cut) for cut in cuts]
            results = [job.result() for job in jobs]
        for cut, expected in results:
            print(f"  [PASS] 断电写点 {cut:2d} -> 重启内容 {expected}，fsck=0")
        print("  [PASS] 跨重启持久化、CoW 快照、系统回滚且 /home 保留")
        print(f"FYXFS 存储集成测试全部通过（日志：{self.work}）")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--work", default=None)
    ap.add_argument("--max-seconds", type=int, default=45)
    args = ap.parse_args()
    work = args.work or str(Path(args.image).parent / "storage-test")
    try:
        StorageTest(args.image, args.kernel, work, args.max_seconds).run_all()
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as exc:
        print(f"storage-test: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
