# FYXOS 远景路线图

> 目标：一套**全新自研**的操作系统 —— 界面比 Windows 10 更好看、兼容性更强、
> 更快更流畅更省资源、体积更小，且**自带全部语言的编译器**、git、以及一个比
> VS Code 更强的 IDE，并提供类似"系统还原"的能力。除游戏外，覆盖一个人日常
> 所需的全部功能。终端同时支持各操作系统的命令，并提供一套**新的、更易用的
> 指令集**。
>
> 本文件是总纲；具体规格见 `docs/spec/`，设计要点见 `docs/design/`。

---

## 0. 当前状态（2026-10）

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 自研引导：MBR(stage1) → stage2 → 长模式 → 解析内核 ELF64 → 跳转 | ✅ 完成 |
| B | 内核核心：GDT/IDT/中断 → PMM → VMM(四级页表) → 堆 → 抢占式调度 → 系统调用 | ✅ 完成 |
| C | 驱动：串口 / 键盘 / PIT 时钟 / VGA 文本 | ✅ 完成 |
| D | 内核 Shell（初版 16 条，现 34 条） | ✅ 完成 |
| E | Unicorn 测试台：假 BIOS + 全链路实跑验证 | ✅ 完成 |
| F | 生态规格与远景设计（本目录） | ✅ 完成 |
| G | 用户态核心：进程/ELF/`fork`/`exec`/`wait`/管道/最小 libc | ✅ 核心完成 |
| H | 存储与文件系统 | ✅ 核心完成（ATA/FYXFS/VFS/断电测试） |
| I | 网络栈 | ⬜ 待办 |
| J | 图形栈与桌面环境（"比 Win10 好看"） | ⬜ 待办 |
| K | 工具链移植（LLVM / GCC / Rust / Go / …）+ git | ⬜ 待办 |
| L | FYX IDE | ⬜ 待办 |
| M | 系统还原 | ✅ 核心完成（CoW 快照/回滚/保留 `/home`） |

验证方式（阶段 E 已建立，后续沿用）：

```bash
python3 -m venv .venv && .venv/bin/pip install unicorn
make image
.venv/bin/python tools/fyxemu.py --image build/fyxos.img \
    --kernel build/fyxos.elf --mode full-boot \
    --keys "help;uname;mem;ps;test;sleep 120;shutdown" \
    --keys-after "[init] 全部通过"
```

`--mode full-boot` 从**第一条 BIOS 指令**开始跑：INT 13h 读盘 → MBR → stage2
（A20/E820/ELF 装载/建页表）→ 长模式 → 内核 banner → boot_info → PMM/VMM/堆
→ 9 项自检 → 抢占调度演示 → ATA/FYXFS → 从 `/bin/init` 装载用户 init
（ring 3、文件 I/O、fork/exec/wait、管道）→ Shell → 正常关机。
`--keys-after` 确保键盘脚本在 init 完成后才注入。

---

## 1. 分阶段路线

### 阶段 G — 用户态基石

- 进程模型：`struct process`（独立页表、句柄表、cwd、凭证）
- ELF64 用户态加载器（复用 stage2 的 ELF 解析逻辑，抽到 `kernel/sys/elf.c`）
- `fork`（写时复制）/ `exec` / `wait` / `exit`
- 用户栈与参数/环境块布局（遵循 SysV ABI）
- libc 起步：`fyx-libc`（malloc、字符串、stdio、errno、locale）
- 用户态系统调用封装（复用已定义的 SYSCALL/SYSRET ABI）

**验收**：测试台上跑起一个用户态 `/bin/init`，它能 `fork`+`exec` 出第二个
用户程序，两个进程靠管道通信。

**当前实现（已实跑通过）**：静态 ELF64 init 从持久化 `/bin/init` 读取并在
ring 3 启动；独立 PML4、用户栈/argv/envp/auxv、每线程系统调用栈、
`fork`/`exec`/`wait`/`exit`/`brk`、管道与文件 fd 均已在 full-boot 与 quick
台架验证。fork 暂为全量复制、管道等待暂为 tick 轮询；进程 CoW、条件变量、
完整用户指针检查及完整 libc 留到后续，不把它们冒充为已完成。

### 阶段 H — 存储与文件系统

- **FYXFS**：写时复制（CoW）日志型文件系统
  - 这是"系统还原"与"小体积"的公共底座：快照 = 一次 CoW 树的根切换
  - 特性： extent 分配、校验和、透明压缩（zstd）、去重（可选）
- 块设备层：AHCI / NVMe / VirtIO-blk
- 分区：GPT；引导沿用现有 BIOS/UEFI 双路径
- VFS：统一的 `vnode` 接口，支持 `procfs` / `sysfs` / `devfs` / `tmpfs`

**验收**：格式化、挂载、读写、卸载、断电一致性测试（台架上模拟掉电）。

**当前实现（已实跑通过）**：自研 ATA PIO、MBR 系统分区、FYXFS v1、VFS
文件对象及 6 个文件系统调用均已落地；`make test-storage` 验证跨重启、
卸载/挂载和 4 个 ATA 写点断电。当前是固定 40 节点/16 快照的追加式 CoW 根，
尚未完成旧块回收、B 树、压缩/去重、GPT、AHCI/NVMe 及 proc/sys/dev/tmpfs。
详见 [`docs/design/fyxfs.md`](design/fyxfs.md)。

### 阶段 I — 网络栈

- 网卡驱动：E1000 / VirtIO-net / RTL8139
- L2：Ethernet、ARP；L3：IPv4/IPv6、ICMP；L4：TCP（拥塞控制用 BBR）、UDP
- 套接字 API + TLS（自带或移植 rustls / mbedTLS）
- DNS 解析、DHCP 客户端

### 阶段 J — 图形栈与桌面

- 显示：线性帧缓冲（启动期从 GOP/BIOS 取得）、多显示器
- 合成器：**FyxWay**（自研合成协议，借鉴 Wayland 的分层思路但更精简）
- 渲染：自研 `fyx-gfx`（GPU 无关的光栅化 + Vulkan 后端）
- 界面语言：**FyxUI** —— 声明式、GPU 合成、120Hz、全局暗色/亮色、亚像素抗锯齿
- 桌面环境 **FyxShell**：全局搜索、虚拟桌面、贴靠布局、窗口规则

### 阶段 K — 工具链与 git

见 [`docs/spec/toolchain-interface.md`](spec/toolchain-interface.md)。
顺序：先自举 binutils+GCC（C/C++）→ LLVM/Clang（覆盖更多语言后端）→
Rust / Go / Zig / Python / Node 等通过统一接口接入 → 移植 git。

### 阶段 L — FYX IDE

见 [`docs/design/ide.md`](design/ide.md)。

### 阶段 M — 系统还原

见 [`docs/design/system-restore.md`](design/system-restore.md)。

**当前实现（已实跑通过）**：O(1) CoW 快照、可跨重启的事务式回滚、快照历史
保留，以及默认只恢复系统并保留当前 `/home`；`rollback ID all` 支持整卷恢复。
A/B 内核槽、独立救援镜像、认证、自动保留策略及包管理器联动仍是后续目标。

---

## 2. 贯穿全局的硬指标

| 指标 | 目标 | 说明 |
|---|---|---|
| 冷启动 | ≤ 3 秒（NVMe） | 并行驱动探测、延迟初始化、内核镜像常驻压缩 |
| 基础安装体积 | ≤ 2 GiB | 对比 Win10 的 20+ GiB；靠 CoW + 压缩 + 按需组件 |
| 内核二进制 | ≤ 4 MiB | 当前存储版约 155 KiB（未压缩、含调试信息） |
| 空闲内存占用 | ≤ 400 MiB | 桌面环境常驻 |
| 输入延迟 | ≤ 1 帧（120Hz 下 ≤ 8.3 ms） | 合成器直写、输入线程优先级最高 |
| 兼容性 | POSIX 完整 + Win32 常用子集 | 见 `docs/spec/fyx-cli.md` |

---

## 3. 自研边界

明确**不使用**任何既有内核代码（Linux/BSD/Windows 等）：内核、引导器、
驱动、文件系统、网络栈、合成器、包管理器、IDE 全部自研。

允许且鼓励**移植**的（属于用户态生态，不是内核）：

- 编译器：LLVM / GCC / Rustc（按 FYX 工具链接口接入）
- 版本控制：git
- 基础库：libc 可借鉴 musl 的设计（重写实现）
- 压缩/加密：zstd / BLAKE3 等算法实现

判断标准：**内核态必须自研；用户态优先自研，生态缺口用移植补齐。**
