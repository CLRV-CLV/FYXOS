# 静态 Linux x86_64 与 gzip/未压缩 amd64 DEB 纵切片

## 结论先行

当前分支已经真实完成下面两条安装路径，而不是只有设计或宿主演示：

```text
标准 .deb（control.tar.gz + data.tar.gz）
  → FYXOS guest 解析 ar → gzip/DEFLATE 解码 → ustar 校验
  → 安装到 FYXFS /usr/bin/fyx-deb-demo
  → 装载包内未修改的静态 glibc x86_64 ET_EXEC
  → ring3 + Linux syscall 分派
  → 输出 LINUX_GLIBC_OK / DEB_PACKAGE_OK → 退出码 23

标准 .deb（control.tar + data.tar）
  → 原有未压缩路径 → /etc/fyx-deb-plain-ok
  → 输出 UNCOMPRESSED_DEB_OK
```

**不能据此声称能运行任意 DEB。** 动态 Linux ELF、xz/zstd、依赖求解和完整
dpkg 生态仍未实现。Win64 是另一条已实跑的限定纵切片，见
[`win64-compat.md`](win64-compat.md)；同样不能外推为任意 EXE。

## 可复现验收

```bash
make image
make test-compat          # 21 项，含 quick 与完整 BIOS
make clean && make test   # 原 80 项 + 兼容 21 项
```

当前干净全量结果：**101 PASS / 0 FAIL / RC=0 / 编译诊断 0**，日志为
`build/full-test-win64-deb.log`。兼容套件覆盖：

1. gzip/未压缩 DEB 的 ar 成员、amd64 control、目标内容与包内 ELF 逐字节一致；
2. 同一 `kernel/lib/gzip.c` 在宿主 ASan/UBSan 下通过 stored、fixed、dynamic、
   可选头字段，以及坏 CRC、坏 ISIZE、截断、尾随、多 member、输出上限负例；
3. 损坏 data.gz CRC 在 guest 返回 EIO，且目标文件在尝试前不存在；
4. 两种 DEB 均在 guest 安装，gzip 包内 ELF 的尺寸/CRC32 与宿主一致；
5. Linux 输出和退出码 23、Win64 输出和退出码 24、原生程序退出码 42；
6. 未知 kernel32 导入、ELF/PE 跨格式和非 DEB 输入均明确拒绝；
7. FYXFS 校验和正常关机通过，quick 与完整 BIOS/stage2 都核对包 CRC 并执行。

## 构建载荷

`compat/deb-demo/hello.c` 是普通 hosted C 源码。Makefile 使用开源 GCC 的
`-static -Os -no-pie` 和 section GC 构建，再以 `dpkg-deb -Zgzip -z9` 生成
标准 amd64 DEB。包内 stripped ELF 为 674,504 字节；宿主只负责产生测试包，
安装与执行都在 guest 内完成。

另有一个很小的 `fyx-deb-plain-demo`，持续覆盖未压缩 tar 路径；
`fyx-deb-bad-gzip.deb` 保持 control 和 DEFLATE 数据有效，只破坏 data 的
CRC32，验证必须在写文件前失败。三者和 Win64 正/负 EXE 由
`kernel/sys/init_image.S` 作为首启种子携带，`storage.c` 按尺寸和 CRC32 写到
VFS。DEB 结束标签位于对齐填充前，VFS 包尺寸与 `dpkg-deb` 产物完全一致。

## Linux ABI 实现

- GNU OSABI 可自动识别，也可用 `run linux` 显式选择；Linux 0..、FYX
  0x1000.. 和 Win32 服务号完全分表。
- ELF 装载提供连续无空洞的 argc/argv/envp/auxv，包括 PHDR、HWCAP、UID/GID、
  CLKTCK、RANDOM、EXECFN。
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

## gzip 与 DEB 安装实现

`kernel/lib/gzip.c` 是自包含 RFC 1951/1952 解码器，不调用宿主 zlib：

- stored/fixed/dynamic DEFLATE、canonical Huffman、距离/长度边界和重叠复制；
- gzip 可选 extra/name/comment/header-CRC 字段；
- 输出上限（当前每个 tar 成员 16 MiB）、实际 ISIZE 和 CRC32 强校验；
- 拒绝过订阅/无效码、越界距离、截断、尾随垃圾和拼接 member。

`kernel/fs/deb.c` 在写入前完整验证 control 和 data tar：

- ar 成员边界/对齐、debian-binary 2.0；
- `control.tar[.gz]` 与 `data.tar[.gz]` 可分别识别；
- ustar 路径、八进制字段、校验和、文件边界；
- `Package`、`Version`、`Architecture: amd64`；
- 拒绝 `Depends`/`Pre-Depends`、maintainer scripts/conffiles；
- 仅解包目录和普通文件，拒绝路径穿越、链接、设备与 pax 扩展。

安装器不是 apt/dpkg：没有仓库、依赖求解、包数据库、权限位、维护者脚本或
完整升级语义。解包写入过程也暂不宣称跨断电事务；已有 FYXFS 单文件原子
替换与安装后的 fsck 不等于整个多文件包事务。

## 完整 BIOS 暴露并修复的布局问题

新增 full-boot 验收曾发现：quick 中 gzip 正常，但 BIOS 路径包 CRC 不同。
逐块校验证明 stage2 的 32 位栈 `0x60000–0x70000` 与增长后的 ELF 暂存区
重叠，段复制时覆盖了压缩流。现在 32 位栈迁到
`0x1F0000–0x1FEFFF`（栈顶 `0x1FF000`），不在暂存区或 2 MiB 内核目标内。
页表池仍只在所有 PT_LOAD 复制完成后使用。该 full-boot CRC/双 ABI 检查已
纳入每次 `test-compat`，不再依赖手工补跑。

## 仍未完成的兼容工作

- 任意 Windows EXE 和完整 Win32：未实现；当前仅见 Win64 文档列出的子集。
- Linux PT_INTERP、动态链接器、共享库、PIE/重定位：未实现，动态输入会拒绝。
- 多线程/futex/clone、完整 signal、mmap 文件映射与完整 POSIX：未实现。
- xz/zstd DEB、依赖、apt/dpkg 数据库和真实 Debian 生态：未实现。
- 完整用户指针验证、Linux 安全语义与完整文件元数据：仍需继续加固。

因此当前准确说法是：**静态 Linux x86_64、标准 gzip/未压缩 amd64 DEB 的
限定纵切片已在 quick 与完整 BIOS 实跑验收；任意 EXE/DEB 仍没有打通。**
