# FYXOS

一套**从零自研**的操作系统：界面比 Windows 10 更好看、兼容性更强、更快更
流畅更省资源、体积更小，并自带全部语言的编译器、git、以及一个比 VS Code 更
强的 IDE，提供系统还原能力。除游戏外，覆盖日常所需的全部功能。终端同时支持
各操作系统的命令，并提供一套新的、更易用的指令集。

**不使用任何既有内核代码。** 内核、引导器、驱动、文件系统、合成器、包管理
器、IDE 全部自研。

---

## 当前进度：阶段 A–E 已完成 ✅

从第一条 BIOS 指令到内核 Shell，全链路在 Unicorn 测试台上实跑验证：

```
BIOS → MBR(stage1) → stage2(A20/E820/ELF 装载/建页表) → 长模式
     → 内核 banner → boot_info → PMM/VMM/堆 → 8 项自检
     → 抢占式调度演示 → Shell → 正常关机
```

### 快速开始

```bash
# 1. 准备测试台（只需一次）
python3 -m venv .venv && .venv/bin/pip install unicorn

# 2. 构建
make image          # = make boot + make kernel + 生成 build/fyxos.img

# 3. 全链路实跑（从 BIOS 第一条指令开始）
.venv/bin/python tools/fyxemu.py --image build/fyxos.img \
    --kernel build/fyxos.elf --mode full-boot \
    --keys "help;uname;mem;ps;test;sleep 120;shutdown"
```

也可以单独跑内核（跳过引导，用于快速迭代）：

```bash
.venv/bin/python tools/fyxemu.py --image build/fyxos.img \
    --kernel build/fyxos.elf --mode quick --keys "ps;shutdown"
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
| 系统调用 | `kernel/sys/` | SYSCALL/SYSRET，16 个调用 |
| 驱动 | `kernel/arch/x86_64/` | 串口、键盘(Set-1 QWERTY)、PIT、VGA 文本 |
| 内核 Shell | `kernel/shell/shell.c` | 16 条命令 |
| 自检 | `kernel/tests/selftest.c` | 8 项，全部通过 |
| 测试台 | `tools/fyxemu.py` | 假 BIOS（INT 10h/13h/15h/16h/1Ah）+ 两段式引擎 |
| 镜像工具 | `tools/mkimage.py` | 生成 32 MiB 磁盘镜像 |

### 目录结构

```
boot/            引导器（stage1 MBR / stage2 16→32→64 位）
include/fyxos/   内核头文件（ABI、boot_info、sched、syscall…）
kernel/          内核（arch/x86_64 · core · lib · mm · sched · shell · sys · tests）
tools/           构建与测试台
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
