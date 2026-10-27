/*
 * FYXOS — SYSCALL / SYSRET 快速系统调用入口
 * ---------------------------------------------------------------------------
 * 进入时（硬件约定）：
 *   RAX = 系统调用号
 *   RDI/RSI/RDX/R10/R8/R9 = 参数 1..6
 *   RCX = 返回地址（被 SYSCALL 覆盖，必须保存）
 *   R11 = RFLAGS（被 SYSCALL 覆盖，必须保存）
 *   RSP 仍是用户栈 —— SYSCALL 不换栈，必须自己切到内核栈
 *
 * 阶段 G 的三个不变式：
 *   1) 每条线程使用自己的内核栈。g_syscall_stack_top 由调度器随当前线程更新，
 *      所以一个线程在 wait()/sleep() 中被换出后，另一个线程不会覆盖它的
 *      C 调用帧。
 *   2) 返回现场保存在这张线程栈上，而不是只放在全局 scratch 中。否则即使
 *      C 栈独立，返回值仍会被后续线程改掉。
 *   3) 除 RAX（返回值）、RCX/R11（ABI 明确破坏）外，所有用户寄存器都必须
 *      原样恢复。内核 C 代码可以自由使用 RBX/R12-R15 等寄存器，不能让它
 *      们的值漏到用户态；用户程序会把长期变量放在这些寄存器里。
 *
 * GS 目前还没有承载 per-CPU 数据，本阶段不执行 swapgs。以后引入 per-CPU
 * 数据时，必须在用户/内核的所有入口（包括中断和线程退出）统一维护 GS，
 * 不能只在这条 fast path 上随意切换。
 */
    .intel_syntax noprefix
    .section .text, "ax"
    .code64
    .globl syscall_entry
    .extern syscall_dispatch
    .extern g_syscall_scratch
    .extern g_syscall_stack_top
    .extern g_user_regs

/* struct user_regs 字段偏移（与 include/fyxos/sched.h 保持一致） */
.equ UR_RAX,      0
.equ UR_RBX,      8
.equ UR_RCX,     16
.equ UR_RDX,     24
.equ UR_RSI,     32
.equ UR_RDI,     40
.equ UR_RBP,     48
.equ UR_RSP,     56
.equ UR_R8,      64
.equ UR_R9,      72
.equ UR_R10,     80
.equ UR_R11,     88
.equ UR_R12,     96
.equ UR_R13,    104
.equ UR_R14,    112
.equ UR_R15,    120
.equ UR_RIP,    128
.equ UR_RFLAGS, 136
.equ UR_CS,     144
.equ UR_SS,     152

/* 线程内核栈上的返回帧 + 一份完整的 struct user_regs */
.equ SF_NUMBER,   8
.equ SF_RIP,     16
.equ SF_RFLAGS,  24
.equ SF_RSP,     32
.equ UR_BASE,    40
.equ FRAME_SIZE, 200                 /* 40 + 160；使 push a6 后栈 16 对齐 */

syscall_entry:
    /* 先保存硬件改写过的值；此时尚未切换栈，不能有任何调用 */
    mov     [g_syscall_scratch + 0],  rax
    mov     [g_syscall_scratch + 8],  rcx
    mov     [g_syscall_scratch + 16], r11
    mov     [g_syscall_scratch + 24], rsp

    /* 切到当前线程自己的内核系统调用栈 */
    mov     rsp, [g_syscall_stack_top]
    sub     rsp, FRAME_SIZE

    /* 把返回现场和完整用户寄存器快照复制到线程栈 */
    mov     rax, [g_syscall_scratch + 0]
    mov     [rsp + SF_NUMBER], rax
    mov     [rsp + UR_BASE + UR_RAX], rax
    mov     rax, [g_syscall_scratch + 8]
    mov     [rsp + SF_RIP], rax
    mov     [rsp + UR_BASE + UR_RCX], rax
    mov     [rsp + UR_BASE + UR_RIP], rax
    mov     rax, [g_syscall_scratch + 16]
    mov     [rsp + SF_RFLAGS], rax
    mov     [rsp + UR_BASE + UR_R11], rax
    mov     [rsp + UR_BASE + UR_RFLAGS], rax
    mov     rax, [g_syscall_scratch + 24]
    mov     [rsp + SF_RSP], rax
    mov     [rsp + UR_BASE + UR_RSP], rax

    mov     [rsp + UR_BASE + UR_RBX], rbx
    mov     [rsp + UR_BASE + UR_RDX], rdx
    mov     [rsp + UR_BASE + UR_RSI], rsi
    mov     [rsp + UR_BASE + UR_RDI], rdi
    mov     [rsp + UR_BASE + UR_RBP], rbp
    mov     [rsp + UR_BASE + UR_R8],  r8
    mov     [rsp + UR_BASE + UR_R9],  r9
    mov     [rsp + UR_BASE + UR_R10], r10
    mov     [rsp + UR_BASE + UR_R12], r12
    mov     [rsp + UR_BASE + UR_R13], r13
    mov     [rsp + UR_BASE + UR_R14], r14
    mov     [rsp + UR_BASE + UR_R15], r15
    mov     QWORD PTR [rsp + UR_BASE + UR_CS],  0x23
    mov     QWORD PTR [rsp + UR_BASE + UR_SS],  0x1b

    /* g_user_regs 是进入 C 前的暂存副本；dispatch 会把它拷到当前线程。
     * 之后若发生切换，返回时只读线程栈，不再读这个全局暂存区。 */
    mov     rax, [rsp + UR_BASE + UR_RAX]
    mov     [g_user_regs + UR_RAX], rax
    mov     rax, [rsp + UR_BASE + UR_RBX]
    mov     [g_user_regs + UR_RBX], rax
    mov     rax, [rsp + UR_BASE + UR_RCX]
    mov     [g_user_regs + UR_RCX], rax
    mov     rax, [rsp + UR_BASE + UR_RDX]
    mov     [g_user_regs + UR_RDX], rax
    mov     rax, [rsp + UR_BASE + UR_RSI]
    mov     [g_user_regs + UR_RSI], rax
    mov     rax, [rsp + UR_BASE + UR_RDI]
    mov     [g_user_regs + UR_RDI], rax
    mov     rax, [rsp + UR_BASE + UR_RBP]
    mov     [g_user_regs + UR_RBP], rax
    mov     rax, [rsp + UR_BASE + UR_RSP]
    mov     [g_user_regs + UR_RSP], rax
    mov     rax, [rsp + UR_BASE + UR_R8]
    mov     [g_user_regs + UR_R8], rax
    mov     rax, [rsp + UR_BASE + UR_R9]
    mov     [g_user_regs + UR_R9], rax
    mov     rax, [rsp + UR_BASE + UR_R10]
    mov     [g_user_regs + UR_R10], rax
    mov     rax, [rsp + UR_BASE + UR_R11]
    mov     [g_user_regs + UR_R11], rax
    mov     rax, [rsp + UR_BASE + UR_R12]
    mov     [g_user_regs + UR_R12], rax
    mov     rax, [rsp + UR_BASE + UR_R13]
    mov     [g_user_regs + UR_R13], rax
    mov     rax, [rsp + UR_BASE + UR_R14]
    mov     [g_user_regs + UR_R14], rax
    mov     rax, [rsp + UR_BASE + UR_R15]
    mov     [g_user_regs + UR_R15], rax
    mov     rax, [rsp + UR_BASE + UR_RIP]
    mov     [g_user_regs + UR_RIP], rax
    mov     rax, [rsp + UR_BASE + UR_RFLAGS]
    mov     [g_user_regs + UR_RFLAGS], rax
    mov     rax, [rsp + UR_BASE + UR_CS]
    mov     [g_user_regs + UR_CS], rax
    mov     rax, [rsp + UR_BASE + UR_SS]
    mov     [g_user_regs + UR_SS], rax

    /* 参数重排：RDI=号，RSI=a1，RDX=a2，RCX=a3，R8=a4，R9=a5 */
    push    r9                      /* a6；压栈后 RSP 16 字节对齐 */
    mov     r9, r8                  /* a5 */
    mov     r8, r10                 /* a4 */
    mov     rcx, rdx                /* a3 */
    mov     rdx, rsi                /* a2 */
    mov     rsi, rdi                /* a1 */
    mov     rdi, [rsp + SF_NUMBER + 8]  /* push 使帧整体下移 8 */

    call    syscall_dispatch        /* 返回值在 RAX */
    add     rsp, 8                  /* 丢弃栈上的 a6，回到返回帧 */

    /* 保存返回值，恢复其余用户寄存器。RCX/R11 按 ABI 用于 sysret */
    mov     [rsp + SF_NUMBER], rax
    mov     rbx, [rsp + UR_BASE + UR_RBX]
    mov     rbp, [rsp + UR_BASE + UR_RBP]
    mov     rdx, [rsp + UR_BASE + UR_RDX]
    mov     rsi, [rsp + UR_BASE + UR_RSI]
    mov     rdi, [rsp + UR_BASE + UR_RDI]
    mov     r8,  [rsp + UR_BASE + UR_R8]
    mov     r9,  [rsp + UR_BASE + UR_R9]
    mov     r10, [rsp + UR_BASE + UR_R10]
    mov     r12, [rsp + UR_BASE + UR_R12]
    mov     r13, [rsp + UR_BASE + UR_R13]
    mov     r14, [rsp + UR_BASE + UR_R14]
    mov     r15, [rsp + UR_BASE + UR_R15]

    mov     rcx, [rsp + SF_RIP]
    mov     r11, [rsp + SF_RFLAGS]
    mov     rax, [rsp + SF_NUMBER]
    mov     rsp, [rsp + SF_RSP]

    sysretq

.section .note.GNU-stack,"",@progbits
