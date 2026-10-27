# 静态 Linux x86_64 与未压缩 amd64 DEB 纵切片

## 结论先行

当前分支已经真实完成下面这条路径，而不是只有设计或宿主演示：

```text
标准 .deb 文件
  → FYXOS guest 解析 ar / control.tar / data.tar
  → 安装到 FYXFS /usr/bin/fyx-deb-demo
  → 装载包内未修改的静态 glibc x86_64 ET_EXEC
  → ring3 + Linux syscall 分派
  → 输出 LINUX_GLIBC_OK / DEB_PACKAGE_OK
  → 退出码 23
```

**不能据此声称能运行任意 EXE 或 DEB。** Windows PE/Win32 尚未实现；动态
Linux ELF、压缩 Debian 包、依赖求解和完整 dpkg 生态也尚未实现。

## 可复现验收

```bash
make image
make test-compat

# 或干净跑原 80 项和新增 9 项
make clean && make test
```

当前干净全量结果：**89 PASS / 0 FAIL / RC=0 / 编译诊断 0**，日志为
`build/full-test-compat.log`。兼容套件的 9 项包括：

1. DEB 恰含 `debian-binary`、`control.tar`、`data.tar`，control 为 amd64；
2. 包内 ELF 与 GCC 构建产物逐字节一致，宿主参考执行输出两行并退出 23；
3. guest 安装后的文件尺寸和 CRC32 与宿主载荷一致；
4. guest 输出 `LINUX_GLIBC_OK`、`DEB_PACKAGE_OK`；
5. guest 报告真实退出码 23；
6. 随后运行原生 `/bin/gcc-c11-demo` 仍输出 `GCC_C11_OK` 并退出 42；
7. 非 DEB 输入明确失败，安装后 `fsck=0`；
8. 正常关机，无 panic、CPU 异常或非法指令；
9. 单调时钟已实现，未知调用边界不含静默成功。

完整 BIOS 路径也单独执行并通过（RC=0）：

```bash
.venv/bin/python tools/fyxemu.py \
  --image build/fyxos.img --kernel build/fyxos.elf --mode full-boot \
  --no-net --no-gfx --keys-after '[init] 全部通过' \
  --keys 'deb /packages/fyx-deb-demo.deb;run linux /usr/bin/fyx-deb-demo;shutdown'
```

对应日志为 `build/compat-full-boot.log`。这次附加 full-boot 验证不重复计入
上述 89 个 `[PASS]`。

## 构建载荷

`compat/deb-demo/hello.c` 是普通 hosted C 源码。Makefile 使用开源 GCC 的
`-static -Os -no-pie` 和 section GC 构建，再用系统 `dpkg-deb -Znone` 生成
标准未压缩 amd64 DEB。当前包为 696,512 字节，包内 stripped ELF 为 674,504
字节。宿主只负责产生测试包；安装与执行都在 guest 内完成。

`kernel/sys/init_image.S` 把 DEB 作为首启种子携带，`storage.c` 按尺寸和
CRC32 更新到 `/packages/fyx-deb-demo.deb`。它不是直接运行内嵌 ELF：验收
必须先经 `deb` 解包，再从安装后的 VFS 路径读回并装载。

## Linux ABI 实现

- GNU OSABI 可自动识别，也可用 `run linux` 显式选择；Linux 0.. 与 FYX
  0x1000.. 的 syscall 编号和 errno 完全分表。
- ELF 装载提供连续无空洞的 argc/argv/envp/auxv，包括 PHDR、HWCAP、UID/GID、
  CLKTCK、RANDOM、EXECFN。修复了对齐 padding 被误认为 `AT_NULL` 的原全局
  bug。
- 每线程保存并恢复 `IA32_FS_BASE`；`arch_prctl` 支持 ARCH_SET_FS/GET_FS。
- 启用 CR4.OSFXSR/OSXMMEXCPT，并为每线程加入 512 字节 FXSAVE 区，抢占切换
  保存/恢复 x87/SSE/SSE2 状态；不宣称 AVX。
- `brk` 区分 Linux 与原生失败语义；匿名 `mmap` 仅支持 PRIVATE|ANONYMOUS，
  另有目标地址空间 `mprotect`/`munmap` 和失败回滚。
- 已实现该载荷路径所需的 read/write/close/lseek、最小 stat/fstat/newfstatat、
  openat 子集、PID/身份查询、`/proc/self/exe` readlink、prlimit 查询、
  exit/exit_group，以及由 PIT 支撑的 monotonic clock_gettime。
- `rt_sigprocmask`、`set_robust_list` 等只覆盖单线程启动约定，不代表已有
  异步信号、robust futex 或 pthread。
- 未识别调用返回 Linux `-ENOSYS`，并按调用号打印一次 PID/nr 诊断。当前
  glibc 会探测 `rseq(334)` 和 `getrandom(318)`：前者禁用后可回退；后者在
  无可靠熵源时明确失败，绝不用 TSC 假随机数冒充安全随机服务。

## DEB 安装实现与限制

`kernel/fs/deb.c` 在写入前先完整验证 control 和 data tar：

- ar 成员边界/对齐、debian-binary 2.0；
- ustar 路径、八进制字段、校验和、文件边界；
- `Package`、`Version`、`Architecture: amd64`；
- 拒绝 `Depends`/`Pre-Depends`、maintainer scripts/conffiles；
- 仅解包目录和普通文件，拒绝路径穿越、链接、设备与 pax 扩展；
- 仅接受未压缩 `control.tar` 和 `data.tar`。

安装器不是 apt/dpkg：没有仓库、依赖求解、包数据库、权限位、维护者脚本或
完整升级语义。解包写入过程也暂不宣称跨断电事务；已有 FYXFS 单文件原子
替换与安装后的 fsck 不等于整个多文件包事务。

## 仍未完成的兼容工作

- 任意 Windows EXE、PE/PE+ 装载、Win32 API：未实现。
- Linux PT_INTERP、动态链接器、共享库、PIE/重定位：未实现，动态输入会拒绝。
- 多线程/futex/clone、完整 signal、mmap 文件映射与完整 POSIX：未实现。
- gzip/xz/zstd DEB、依赖、apt/dpkg 数据库和真实 Debian 生态：未实现。
- 完整用户指针验证、Linux 安全语义与完整文件元数据：仍需继续加固。

因此当前准确说法是：**一个严格限定、可构建且已实跑验收的静态 Linux
x86_64＋标准未压缩 amd64 DEB 纵切片已经打通；任意 EXE/DEB 仍没有打通。**
