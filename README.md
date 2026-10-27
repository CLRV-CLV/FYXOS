# FYXOS

一套**从零自研**的操作系统：界面比 Windows 10 更好看、兼容性更强、更快更
流畅更省资源、体积更小，并自带全部语言的编译器、git、以及一个比 VS Code 更
强的 IDE，提供系统还原能力。除游戏外，覆盖日常所需的全部功能。终端同时支持
各操作系统的命令，并提供一套新的、更易用的指令集。

**不使用任何既有内核代码。** 内核、引导器、驱动、文件系统、合成器、包管理
器、IDE 全部自研。

---

## 当前交付：FYXOS Developer Preview（自研内核＋开源 GCC C11 SDK）✅

从第一条 BIOS 指令到持久化文件系统、用户态进程、IPv4/UDP 网络和快照回滚，
全链路在 Unicorn 测试台上实跑验证：

```
BIOS → MBR(stage1) → stage2(A20/E820/ELF 装载/建页表) → 长模式
     → 内核 banner → boot_info → PMM/VMM/堆 → 9 项自检
     → 抢占式调度 → ATA/FYXFS 挂载 → RTL8139/DHCP/ARP/ICMP
     → 从 /bin/init 装载用户 ELF → ring 3 UDP/文件 I/O / fork / exec / 管道
     → ifconfig/ping/救援 Shell / 快照回滚
     → BGA LFB → 软件背 buffer → 三窗口桌面/主题/焦点 → PNG 帧校验
     → FYXFS C 源码 → 内置 fyxcc → x86_64 ELF → ring3 运行/退出码
     → 开源 GCC C11 SDK → 独立 /bin/gcc-c11-demo → ring3 输出与退出码 42
     → FYX Studio 编辑/保存/编译/运行 → 正常关机；另有跨重启与 ATA 写点断电恢复测试
```

### 快速开始

```bash
# 1. 准备测试台（只需一次）
python3 -m venv .venv && .venv/bin/pip install unicorn

# 2. 构建可启动镜像与开源 GCC C11 SDK
make image          # build/fyxos.img；预装 /bin/gcc-c11-demo
make sdk            # build/fyxos-sdk/ 与 build/fyxos-sdk.tar.gz
./build/fyxos-sdk/bin/fyx-gcc sdk/examples/c11-demo.c -o build/my-demo.elf
# 可选：把自己的 GCC ELF 预装进新镜像（guest 路径仍为 /bin/gcc-c11-demo）
make app-image APP=build/my-demo.elf

# 3. 全链路实跑（从 BIOS 第一条指令开始）
.venv/bin/python tools/fyxemu.py --image build/fyxos.img \
    --kernel build/fyxos.elf --mode full-boot \
    --keys "help;uname;mem;ps;test;desktop;desktop next;desktop theme;ifconfig;ping;sleep 120;shutdown" \
    --keys-after "[init] 全部通过"
```

进入 Shell 后执行 `run /bin/gcc-c11-demo`，应看到 `GCC_C11_OK` 和退出码 42。
这个程序由配套 SDK 的开源 GCC 编译，使用指针、数组、结构体、枚举、函数指针
和 `_Static_assert`，不是内置 fyxcc 的子集产物。SDK 当前在宿主编译 FYXOS ELF；
不把宿主 GCC 描述为已经在 guest 内运行。

也可以单独跑内核（跳过引导，用于快速迭代）：

```bash
.venv/bin/python tools/fyxemu.py --image build/fyxos.img \
    --kernel build/fyxos.elf --mode quick --keys "ps;shutdown" \
    --keys-after "[init] 全部通过"
```

### 已实现

| 模块 | 位置 | 说明 |
|---|---|---|
| 引导 stage1 | `boot/stage1.S` | MBR，512 字节，读 stage2 并跳 `0000:8000` |
| 引导 stage2 | `boot/stage2_16.S`、`boot/stage2_32.c` | A20、E820、INT 13h 扩
展读、解析 ELF64、构造四级页表、进入长模式 |
| GDT/TSS | `kernel/arch/x86_64/gdt.c` | 含 far-ret 重载 CS 的关键修复 |
| IDT/中断 | `kernel/arch/x86_64/idt.c`、`isr.S` | 256 向量、异常解码、IRQ0-15、0x81 调度软中断 |
| 物理内存 | `kernel/mm/pmm.c` | 位图分配器 |
| 虚拟内存 | `kernel/mm/vmm.c` | 四级页表，内核高半核 + HHDM |
| 堆 | `kernel/mm/heap.c` | 首次适应 |
| 调度 | `kernel/sched/` | 抢占式轮转 + idle 线程 + 上下文切换 |
| 系统调用 | `kernel/sys/` | SYSCALL/SYSRET，38 个调用，每线程独立系统调用栈 |
| 用户进程 | `kernel/sys/process.c`、`elf_load.c`、`usermode.S` | 独立页表、ring 3、fork/exec/wait/exit/brk；从 VFS 装载 ELF |
| 管道与 fd | `kernel/sys/pipe.c` | 管道/文件/UDP socket 统一 fd，fork 引用、跨 exec、EOF |
| ATA 块设备 | `kernel/drivers/ata.c` | 自研 ATA IDENTIFY、LBA28 PIO 读写、FLUSH |
| RTL8139 网络 | `kernel/drivers/rtl8139.c`、`kernel/net/` | ARP/IPv4、DHCP/ICMP、UDP checksum、DNS 重传/TTL 缓存/TCP 回退与 ring3 socket |
| BGA 图形栈 | `kernel/drivers/bga.c`、`kernel/gfx/` | 800×600×32 LFB、VMM NOCACHE 映射、软件背 buffer、光栅/字体、present |
| 首版桌面 | `kernel/gfx/desktop.c` | 三窗口焦点/z-order、阴影/dock、明暗主题、键盘交互 |
| 开源 GCC C11 SDK | `sdk/`、`tools/mksdk.py` | 宿主 GCC 完整 C11 前端、CRT/链接脚本、最小运行库、SDK 压缩包与预装 ring3 示例 |
| 原生 C 子集工具链 | `kernel/toolchain/fyxcc.c` | 局部整数变量、算术/位/逻辑/?: 运算、复合赋值、while/do/for；直接生成静态 x86_64 ET_EXEC |
| FyxJS 子集工具链 | `kernel/toolchain/fyxjs.c` | 多个静态整数 function、参数/递归、let、严格比较、console.log；共用原生后端生成 ELF |
| FYX Studio 首版 | `kernel/ide/ide.c` | 真实文本缓冲、基本高亮、Ctrl-S/B/R/Q 保存/编译/运行/退出；运行仍是独立 ring3 进程 |
| FYXGit v1 | `kernel/git/fyxgit.c` | 原子仓库数据库、暂存/提交父链、状态、文本 diff、restore；不是 Git SHA-1/pack 兼容实现 |
| FYXFS | `kernel/fs/fyxfs.c` | 4KiB CoW 块、双超级块、CRC32、目录/extents、快照 |
| VFS | `kernel/fs/vfs.c` | 路径/目录、文件对象、共享偏移、open/read/write/lseek、原子 rename |
| 系统还原 | `kernel/fs/fyxfs.c` | O(1) 快照、事务式回滚，默认保留 `/home` |
| 用户程序 | `user/` | 静态 ELF64、最小 libc 封装与文件/进程验收 init |
| 驱动 | `kernel/arch/x86_64/` | 串口、键盘(Set-1 QWERTY)、PIT、VGA 文本 |
| 内核 Shell | `kernel/shell/shell.c` | 65 条命令，含 FYX 原生动词、desktop、ide、fyxcc/fyxjs/run、git、resolve/ifconfig/ping 与跨系统子集别名 |
| 自检 | `kernel/tests/selftest.c` | 9 项，全部通过 |
| 测试台 | `tools/fyxemu.py`、`tools/rtl8139.py`、`tools/bga.py` | 假 BIOS + 可写 ATA + 网络对端 + BGA/PNG + 断电注入 + 两段式引擎 |
| 镜像工具 | `tools/mkimage.py` | 生成 32 MiB 磁盘镜像 |

### 自动化验收

```bash
make test           # full/quick 各 25 + SDK 1 + 网络 6 + 工具链/IDE 11 + Git/CLI 4 + 图形 3 + 存储 5
make test-sdk      # GCC 独立编译、ELF64 W^X、预装示例 ring3 运行
make test-network   # DNS 重传/TTL 缓存、TCP 重传、UDP checksum 与 ICMP
make test-toolchain  # C/FyxJS、循环/运算符、诊断与 Studio/PNG 闭环
make test-git        # 提交、diff、restore 与跨重启仓库持久化
make test-cli        # 原生动词、跨系统别名、rename 与跨重启检查
make test-graphics  # 交互、PNG 像素、帧校验和、无 BGA 救援路径
make test-storage   # 只重跑存储集成测试
```

断电测试在第 1、9、16、17 个 ATA 写扇区处分别停机并保留介质，再用同一镜像
重新启动，验证旧/新事务内容且 `fsck=0`。状态镜像和日志放在
`build/storage-test/`。

图形首版仅支持 BGA 800×600 软件光栅化；没有 GPU/Vulkan、多显示器、鼠标、
用户态窗口协议、完整字体/输入法和真实 RTC。桌面是内核态首版合成器，不能据此
宣称完整 FyxWay/FyxUI 或 120Hz。fyxcc 目前支持多个 `int` 函数和一个无参
`int main`，每函数最多 16 个局部/参数整数、4 个栈传参，支持嵌套与递归调用、
函数原型、前向调用和相互递归；被调用函数必须先声明或定义。表达式支持
算术/比较/移位/位/逻辑运算、复合赋值、局部变量 `++/--`、`if/else`、
`while/do…while/for`、`switch/case/default`、`break/continue`、`?:` 和
`puts/print`；switch 支持整数字面量标签与 fallthrough。没有函数指针、指针、数组、
结构体、预处理器或 libc，不是完整 C/GCC/Clang。FyxJS 同样仅覆盖静态整数子集，
现已支持多个函数、最多 4 个参数和递归，但调用前必须先定义，没有函数提升；
`process.exit` 仍只在 main 中可用。没有动态类型、对象、闭包、数组、事件循环
或 JS 库；完整多语言工具链仍未完成。Studio 也是单文件、末尾插入式首版，不是 VS Code 级 IDE。
FYXGit 仅是上述受限 v1，不是完整 Git 兼容；完整 TCP/IPv6、A/B 槽与独立救援
环境仍未完成。

当前刻意保留的简化：fork 仍全量复制用户页；init 的安装种子仍由 `.incbin`
携带（但启动与 exec 已从 `/bin/init` 读盘）；管道等待采用 tick 轮询；libc 仍是
最小封装。网络支持 RTL8139 轮询、IPv4、DHCP、ICMP、UDP checksum、DNS A（一次
UDP 重传、8 项 TTL 缓存）、主动 TCP 客户端及 DNS 截断后的 TCP 回退；TCP 仅有固定间隔的 SYN/单段
数据重传，尚无 listen/server、自适应 RTO、拥塞控制和乱序重组，也尚无 IPv6、
TLS、DNS AAAA/CNAME 递归/负缓存和 IP 分片重组。
FYXFS v1 限 40 节点/16 快照，采用追加分配，尚未实现旧块回收、B 树、压缩、
去重、GPT、AHCI/NVMe 和权限位；
这些边界不会被算作已完成。

### 目录结构

```
boot/            引导器（stage1 MBR / stage2 16→32→64 位）
include/fyxos/   内核头文件（ABI、boot_info、sched、syscall…）
kernel/          内核（arch/x86_64 · core · drivers · gfx · lib · mm · net · sched · shell · sys · tests）
user/            用户态最小 libc、_start、init 验收程序与链接脚本
sdk/             开源 GCC C11 SDK 包装、freestanding 运行库与完整语言示例
tools/           构建、SDK 打包与测试台
docs/            规格与远景设计
```

### 关键地址约定

| 用途 | 地址 |
|---|---|
| 内核物理基址 | `0x200000` |
| 内核虚拟基址 | `0xFFFFFFFF80000000` |
| HHDM 窗口 | `0xFFFF800000000000` |
| boot_info | `0x5000` |
| ELF 暂存区（实模式可达） | `0x10000` – `0x4FFFF` |
| 引导页表池 | `0x50000` |
| 系统调用号起始 | `0x1000` |

---

## 后续

见 [`docs/roadmap.md`](docs/roadmap.md)。规格与设计：

- [`docs/spec/fyx-package.md`](docs/spec/fyx-package.md) — `.fyx` 包格式
- [`docs/spec/toolchain-interface.md`](docs/spec/toolchain-interface.md) — 工具链接口
- [`docs/spec/fyx-cli.md`](docs/spec/fyx-cli.md) — 终端：跨系统命令兼容 + 新指令集
- [`docs/design/ide.md`](docs/design/ide.md) — FYX IDE
- [`docs/design/system-restore.md`](docs/design/system-restore.md) — 系统还原
- [`docs/design/user-mode.md`](docs/design/user-mode.md) — 阶段 G 用户态实现与验证边界
- [`docs/design/fyxfs.md`](docs/design/fyxfs.md) — FYXFS 事务、断电测试与系统还原实现
- [`docs/design/network.md`](docs/design/network.md) — RTL8139、IPv4/UDP/TCP/DNS、异常报文与实跑验收边界
- [`docs/design/graphics.md`](docs/design/graphics.md) — BGA、软件光栅、桌面交互与帧校验边界
- [`docs/design/fyxgit.md`](docs/design/fyxgit.md) — FYXGit v1 仓库格式、diff/restore 与跨重启边界
- [`docs/design/fyxjs.md`](docs/design/fyxjs.md) — FyxJS 原生 JavaScript 子集前端与边界
