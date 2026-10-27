# FYXOS 0.1.0 Developer Preview 1

首个可启动、可开发、可实跑验收的产品包。自研 BIOS 引导器、x86_64 内核、
FYXFS、ring3、网络、桌面和内置工具链保持不变；首版完整语言开发改用开源
GCC C11 SDK，不再等待自研 C 子集扩张。

由于当前环境无法连接 GitHub 独立资产上传域，产品文件直接版本化在本目录；
release 页面提供对应源码标签，不把未上传的文件称为 release 附件。

## 产物

- `fyxos-0.1.0-dev1-x86_64.img.gz`：gzip 压缩的 32 MiB、65536 扇区 x86_64 BIOS 磁盘镜像；先用 `gzip -d` 解压。
- `fyxos-0.1.0-dev1-sdk.tar.gz`：宿主端 GCC C11 SDK、CRT、链接脚本、最小
  freestanding 运行库、示例源码及预编译示例。
- `full-test.log`：干净全量 **80 PASS / 0 FAIL / RC=0**，编译诊断 0。
- `boot-test.log`：发布镜像副本从 BIOS 第一条指令启动，运行 GCC 示例的证据。
- `app-image-test.log`：自定义 hello 经 SDK 编译、打包、启动并退出 0 的证据。
- `fyxos-developer-preview-launch.mp4`：180 秒、1920×1080、24 fps 中文发布片，
  H.264＋立体声 AAC；使用真实截图/日志与原创合成配乐。
- `video-verification.json`：最终 MP4 的 4320 帧、全流解码、音轨和 SHA-256 验收。
- 可编辑 Remotion 工程位于源码树 `video/`，复现命令见 `video/README.md`。

## 产品文件

- [压缩启动镜像](https://github.com/CLRV-CLV/FYXOS/blob/v0.1.0-dev1/release/fyxos-0.1.0-dev1-x86_64.img.gz)
- [GCC C11 SDK](https://github.com/CLRV-CLV/FYXOS/blob/v0.1.0-dev1/release/fyxos-0.1.0-dev1-sdk.tar.gz)
- [SHA256 校验和](https://github.com/CLRV-CLV/FYXOS/blob/v0.1.0-dev1/release/SHA256SUMS.txt)
- [完整测试日志](https://github.com/CLRV-CLV/FYXOS/blob/v0.1.0-dev1/release/full-test.log)
- [三分钟发布片](fyxos-developer-preview-launch.mp4)（当前交付分支）
- [Remotion 源工程](../video/README.md)（当前交付分支）

## 已验证的产品路径

1. 镜像启动后执行 `run /bin/gcc-c11-demo`，输出 `GCC_C11_OK`，退出码 42。
   示例使用指针、数组、结构体、枚举、函数指针和 `_Static_assert`。
2. 解压 SDK 后，在具有 x86_64 ELF GCC 的 Linux 宿主执行：
   `bin/fyx-gcc examples/c11-demo.c -o c11-demo.elf`。
3. 在源码树根目录执行 `make app-image APP=build/my-app.elf`，把自己的 GCC ELF
   预装进新镜像；guest 中仍通过 `/bin/gcc-c11-demo` 运行。

## 明确边界

这是 Developer Preview，不是最终完整 OS。GCC 当前在宿主编译，并非 guest 内
自举；目标是 freestanding FYXOS ELF，不是 Linux/glibc 兼容层。尚无 hosted
libc、动态链接、浮点/SSE 用户上下文、全部语言、完整桌面或完整 POSIX。
内置 fyxcc/FyxJS 仍按其实际子集能力单独说明。

源标签：`v0.1.0-dev1`（当前分支的完整产品提交）

解压后原始镜像 SHA256：`542f1e1886bcde17a3c862227f3753f8785d2306fa6be54c456da9e461b31d01`。
