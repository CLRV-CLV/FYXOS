#!/usr/bin/env python3
"""Build a self-contained FYXOS GCC SDK tree, archive, and native demo ELFs."""
from __future__ import annotations

import argparse
import gzip
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def run(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=ROOT, check=True)


def write_archive(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w") as archive:
                for path in sorted(source.rglob("*")):
                    relative = path.relative_to(source)
                    info = archive.gettarinfo(str(path), f"fyxos-sdk/{relative}")
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = 0
                    if path.is_file():
                        with path.open("rb") as content:
                            archive.addfile(info, content)
                    else:
                        archive.addfile(info)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--demo", required=True, type=Path)
    parser.add_argument("--hello", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT or output == Path(output.anchor):
        raise SystemExit("mksdk: refusing unsafe output directory")
    marker = output / ".fyxos-sdk-generated"
    if output.exists():
        if not marker.is_file():
            raise SystemExit("mksdk: output exists but is not a generated SDK tree")
        shutil.rmtree(output)
    shutil.copytree(ROOT / "sdk", output)
    marker.write_text("FYXOS generated SDK; safe to replace by mksdk.py\n")
    (output / "bin" / "fyx-gcc").chmod(0o755)
    shutil.copy2(ROOT / "user" / "fyx.h", output / "include" / "fyx.h")
    shutil.copy2(ROOT / "user" / "user.ld", output / "lib" / "fyxos-user.ld")

    cc = os.environ.get("FYX_HOST_CC", "gcc")
    ar = os.environ.get("AR", "ar")
    crt_flags = [
        "-ffreestanding", "-fno-pie", "-fno-stack-protector", "-m64",
        "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
        "-mno-80387", "-mno-avx", "-Wa,--noexecstack",
    ]
    run([cc, *crt_flags, "-c", str(ROOT / "user" / "start.S"),
         "-o", str(output / "lib" / "crt0.o")])
    compiler = str(output / "bin" / "fyx-gcc")
    run([compiler, "-c", str(output / "lib" / "minilibc.c"),
         "-o", str(output / "lib" / "minilibc.o")])
    run([ar, "rcs", str(output / "lib" / "libfyx.a"),
         str(output / "lib" / "minilibc.o")])
    (output / "lib" / "minilibc.o").unlink()

    for source, destination in ((output / "examples" / "c11-demo.c", args.demo),
                                (output / "examples" / "hello.c", args.hello)):
        destination.parent.mkdir(parents=True, exist_ok=True)
        run([compiler, str(source), "-o", str(destination)])
        shutil.copy2(destination, output / "examples" / destination.name)
    write_archive(output, args.archive)
    print(f"SDK: {output}")
    print(f"Archive: {args.archive}")
    print(f"C11 demo: {args.demo}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
