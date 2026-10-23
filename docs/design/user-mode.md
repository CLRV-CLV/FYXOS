# FYXOS 用户态实现说明（阶段 G）

本文记录当前已落地的用户态链路，以及有意保留的简化。它是实现说明，不把
后续目标写成既成事实。

## 1. 已验证的执行链

```
内核 kmain
  → ATA/FYXFS 挂载，必要时安装初始 /bin/init
  → process_start_init() → vfs_read_file("/bin/init")
  → process_load_elf()       为 init 建独立 PML4、代码/数据页和用户栈
  → process_thread_entry()   切 CR3、设置 TSS.RSP0
  → arch_return_to_user()    sysret 到 ring 3 的 _start
  → init: fork → 管道写入 → exec → wait → brk
  → init exit → 调度回内核 Shell
```

`make test` 在 full-boot 和 quick 两种模式都执行这条链，并检查：
内核自检、调度演示、ring 3 输出、fork 返回值、exec 新参数、wait 退出码、
管道内容、堆增长和正常关机。

## 2. 地址空间与 ELF

- 用户空间使用 PML4 低半部分，内核高半核页表项共享；内核因此在任意进程
  上下文都能运行。
- 当前只接受静态、固定地址的 ELF64 `ET_EXEC`，逐 PT_LOAD 页映射，按段设置
  `VMM_USER`、写权限和 NX，遵循 W^X。
- `process_load_elf()` 按 SysV AMD64 ABI 布置 `argc/argv/envp/auxv`；只有程序
  头表确实被映射时才写 `AT_PHDR`，否则使用精简 auxv。
- init 的安装种子仍用 `.incbin` 随内核携带，但只用于首启写入 `/bin/init`；
  `process_start_init()` 和 `exec` 已统一通过 VFS 读取文件，不再直接执行内嵌
  blob，也没有另写一套 ELF 解析器。

## 3. 系统调用与调度不变式

- ABI：RAX 为调用号，参数为 RDI/RSI/RDX/R10/R8/R9；RCX/R11 按硬件约定
  保存返回 RIP/RFLAGS。
- 每条线程有独立内核栈。`g_syscall_stack_top` 随调度切换，不能在多个线程
  间共用一张系统调用栈。
- 返回现场和完整通用寄存器快照也保存在当前线程栈上；除 RAX、RCX、R11 外，
  系统调用必须恢复用户寄存器。
- 每次切换用户线程同时更新 CR3 和 TSS.RSP0，保证 ring 3 中断能落到正确的
  内核栈。
- 当前没有 per-CPU GS 数据，暂不执行 swapgs；引入 per-CPU 数据时必须在中断、
  系统调用和线程退出所有路径统一设计，不可只补一条 fast path。

## 4. fork / exec / wait

- fork 当前为全量复制用户页（正确优先，CoW 后续优化），复制父进程用户现场，
  子进程 RAX 改为 0；fd 表复制并增加管道端点或文件对象引用。
- exec 先把名字、argv、envp 复制到内核栈，再从 `/bin/<name>` 或显式路径读取
  ELF，在新地址空间试装载，成功后才交换地址空间并回收旧页表；失败不会毁掉
  原程序。fd 表保留。
- wait 只等待当前进程的子进程，子进程退出立即关闭 fd；父进程回收时释放
  地址空间。

## 5. 管道

`kernel/sys/pipe.c` 提供固定容量的内核环形缓冲：

- `SYS_PIPE` 安装读/写 fd，`SYS_READ`/`SYS_WRITE` 操作当前进程 fd；
- fork 继承、exec 保留，最后一个写端关闭后读端看到 EOF；
- 暂时用 1 tick 睡眠重试代替条件变量；fd 已与 VFS 文件统一分派，后续再
  引入睡眠队列/条件变量消除轮询。

## 6. 测试台边界

Unicorn 2.1.4 对 SYSCALL/SYSRET、ring 3 iretq 和手工中断投递并不完整。
`tools/fyxemu.py` 只在台架侧补齐这些硬件语义：SYSCALL 钩子、SYSRET 仿真、
TSS 栈切换、用户态 iretq 弹帧和异常帧构造。内核仍按真机 ABI 编写，不能为了
迁就台架而改成假的用户态。

full-boot 只传 `--image` 时，台架会从 MBR 的 `FYXB` 描述符提取内核 ELF，
因此符号解析和上述仿真仍然可用。
