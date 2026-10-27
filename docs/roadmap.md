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
| D | 内核 Shell（初版 16 条，现 65 条） | ✅ 完成 |
| E | Unicorn 测试台：假 BIOS + 全链路实跑验证 | ✅ 完成 |
| F | 生态规格与远景设计（本目录） | ✅ 完成 |
| G | 用户态核心：进程/ELF/`fork`/`exec`/`wait`/管道/最小 libc | ✅ 核心完成 |
| H | 存储与文件系统 | ✅ 核心完成（ATA/FYXFS/VFS/断电测试） |
| I | 网络栈 | 🟡 IPv4/UDP、DNS A 与 TCP 主动客户端 v1；IPv6/TLS/完整 TCP 待办 |
| J | 图形栈与桌面环境（"比 Win10 好看"） | 🟡 BGA 软件桌面核心完成；完整 FyxWay/FyxUI 待办 |
| K | 工具链移植（LLVM / GCC / Rust / Go / …）+ git | 🟡 C/FyxJS 子集与 FYXGit v1 完成；完整工具链/Git 待办 |
| L | FYX IDE | 🟡 单文件 Studio 编辑/构建/运行完成；完整 IDE 待办 |
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

**当前实现（已实跑通过）**：RTL8139 PCI/Bus Master DMA、Ethernet/ARP/IPv4、
DHCP、ICMP echo、UDP、socket fd，以及 `ifconfig`/`ping` 已接入 full-boot 与
quick 验收；ring3 init 会向台架 UDP/9 echo 服务发送并校验真实回应。DNS v1
复用同一套内核 UDP socket/队列，从 DHCP option 6 取得服务器，`resolve` 与
`ping <hostname>` 已验证 A 记录、错误事务 ID、压缩名、NXDOMAIN、畸形报文和
超时。TCP 主动客户端复用原有 5 个 socket syscall，ring3 已实测 SYN/SYN-ACK/
ACK、RST 拒绝、伪首部 checksum、PSH echo、流式部分读取、FIN 与 EOF；SYN
和单个未确认数据段支持固定 20ms、最多三次发送，UDP 也会生成并校验伪首部
checksum。DNS 在 UDP TC 后通过同一 TCP 客户端发送长度前缀查询并解析响应；
UDP 无响应时以同一事务 ID 重传一次，8 项 A 记录缓存遵循 TTL 并规范化大小写
和根点。当前共 38 个 syscall、65 条 Shell 命令。listen/server、自适应 RTO、
拥塞控制、乱序重组、IPv6、TLS、DNS AAAA/CNAME 递归/负缓存与 E1000/VirtIO-net 仍未实现，
详见 [`docs/design/network.md`](design/network.md)。

### 阶段 J — 图形栈与桌面

**当前实现（已实跑通过）**：`kernel/drivers/bga.c` 在运行时探测 BGA，设置
800×600×32 并通过 VMM 映射 `0xE0000000` LFB；`kernel/gfx/gfx.c` 提供连续物理
背 buffer、裁剪光栅基元、内置 5×7 字体和 present/FNV-1a；`desktop.c` 实现三
个重叠窗口、焦点 z-order、明暗主题、dock 与 N/T/R/Q 键盘交互。full-boot、
quick、PNG 像素和 guest/host 帧校验均已接入验收，详见
[`docs/design/graphics.md`](design/graphics.md)。

**仍未完成**：

- 显示：GOP/VBE 启动期 framebuffer、多显示器、模式枚举与热插拔
- 合成器：**FyxWay** 用户态协议、窗口管理和真正的客户端生态
- 渲染：GPU/Vulkan 后端、DMA-buf、垂直同步和 120Hz 验证
- 界面语言：**FyxUI** 声明式组件、亚像素抗锯齿、完整 Unicode/输入法
- 桌面环境 **FyxShell**：鼠标、全局搜索、虚拟桌面、贴靠布局、窗口规则

首版是内核态软件桌面，不把上述目标或“比 Win10 完整桌面”算作已交付。

### 阶段 K — 工具链与 git

见 [`docs/spec/toolchain-interface.md`](spec/toolchain-interface.md)。
顺序：先自举 binutils+GCC（C/C++）→ LLVM/Clang（覆盖更多语言后端）→
Rust / Go / Zig / Python / Node 等通过统一接口接入 → 移植 git。

**当前实现（已实跑通过）**：`fyxcc` 在内核中把小型 C 子集直接编译为静态
x86_64 ET_EXEC，经 FYXFS 保存后由 `run`/IDE 装载到 ring3；支持单个 `int main`、
最多 16 个栈上局部整数变量、赋值、四则/比较、`if/else`、`while`、嵌套块和
`puts/print`，重复编译、真/假分支、循环、真实退出码和非法输入诊断均有自动化
验收。它目前不是完整 C。`fyxjs`/`jsc` 另提供 FyxJS v1 前端：`function main`、
`let`、严格比较、`console.log` 和 `process.exit` 已生成同一种原生 ELF 并实跑
返回 42；动态类型、对象、闭包、数组、事件循环和 JS 库未实现，完整多语言
工具链仍是后续目标。
另有 FYXGit v1：单个原子持久化 `.fyxgit` 数据库、暂存区、提交父链、文本 diff
和 restore 已跨重启验收；它不是 Git 的 SHA-1/index/pack 兼容实现，完整 git
移植、分支/合并/克隆和远程协议仍未完成。

### 阶段 L — FYX IDE

见 [`docs/design/ide.md`](design/ide.md)。

**当前实现（已实跑通过）**：FYX Studio 使用 BGA 软件图形栈显示单文件缓冲区，
支持基本语法着色、行号、末尾插入/退格、Ctrl-S 保存、Ctrl-B 编译、Ctrl-R 运行、
Ctrl-Q 退出；`tools/test_toolchain.py` 验证从空文件到磁盘 ELF、ring3 输出和 PNG
校验和的完整闭环。多光标、任意位置编辑、LSP、调试器、扩展和 git 集成仍未完成。

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
