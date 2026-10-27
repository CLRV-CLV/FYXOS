/*
 * FYXOS — 返回用户态（ring 3）
 * ---------------------------------------------------------------------------
 * arch_return_to_user(const struct user_regs *r)
 *
 * 这是所有"回到用户态"路径的唯一出口：
 *   - init / exec：现场是构造出来的，rip 指向 _start；
 *   - fork 的子进程：现场是父进程的一份拷贝，rax 改成 0；
 *   - 普通系统调用返回：走 syscall_entry.S 里的 sysretq。
 *
 * 用 sysretq 而不是 iretq：sysret 正是为"从快速系统调用返回"设计的，它按
 * STAR 算出 CS/SS（见 gdt.c 里 GDT_STAR_USER_BASE 的说明），比手工压 iret
 * 帧更不容易出错。代价是 CS/SS 不可选 —— 内核用不到用户自定义的代码段，
 * 因此 uregs 里的 cs/ss 只作记录用。
 *
 * 这里**不做** swapgs：本函数也会被"从未经历过 syscall 陷入"的新线程调用
 * （init、fork 子进程），它们的 GS 还停在用户态那份上，再做一次 swapgs 反而
 * 会把内核的 GS 留给用户态。swapgs 只在 syscall_entry 的进出路径上成对出现。
 */
    .intel_syntax noprefix
    .section .text, "ax"
    .code64
    .globl arch_return_to_user

/* struct user_regs 的字段偏移，必须与 include/fyxos/sched.h 保持一致 */
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

arch_return_to_user:
    /* 先恢复不会被 sysret 用到的寄存器 */
    mov     rbx, [rdi + UR_RBX]
    mov     rbp, [rdi + UR_RBP]
    mov     r12, [rdi + UR_R12]
    mov     r13, [rdi + UR_R13]
    mov     r14, [rdi + UR_R14]
    mov     r15, [rdi + UR_R15]
    mov     r8,  [rdi + UR_R8]
    mov     r9,  [rdi + UR_R9]
    mov     r10, [rdi + UR_R10]
    mov     rdx, [rdi + UR_RDX]
    mov     rsi, [rdi + UR_RSI]

    /* sysret 的两个硬件约定：RCX = 返回地址，R11 = RFLAGS */
    mov     rcx, [rdi + UR_RIP]
    mov     r11, [rdi + UR_RFLAGS]

    mov     rsp, [rdi + UR_RSP]
    mov     rax, [rdi + UR_RAX]

    /* rdi 最后恢复 —— 它一直当着结构体指针用 */
    mov     rdi, [rdi + UR_RDI]

    sysretq

.section .note.GNU-stack,"",@progbits
