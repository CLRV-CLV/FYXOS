/*
 * FYXOS — SYSCALL / SYSRET 快速系统调用入口
 * ---------------------------------------------------------------------------
 * 进入时（硬件约定）：
 *   RAX = 系统调用号
 *   RDI/RSI/RDX/R10/R8/R9 = 参数 1..6
 *   RCX = 返回地址（被 SYSCALL 覆盖，必须保存）
 *   R11 = RFLAGS（被 SYSCALL 覆盖，必须保存）
 *
 * 本阶段还没有用户态，因此暂不执行 swapgs；相关位置已用注释标出，
 * 阶段 3 引入用户进程时补上即可（配合 MSR_GS_BASE / MSR_KERNEL_GS_BASE）。
 *
 * 参数重排不用任何暂存寄存器，避免破坏用户上下文：
 *   push r9 ; r9<-r8 ; r8<-r10 ; rcx<-rdx ; rdx<-rsi ; rsi<-rdi ; rdi<-rax
 */
    .intel_syntax noprefix
    .section .text, "ax"
    .code64
    .globl syscall_entry
    .extern syscall_dispatch
    .extern g_syscall_scratch
    .extern g_syscall_stack

#define SCRATCH_SIZE 64
#define STACK_QWORDS 1024

syscall_entry:
    /* 先把会被破坏的寄存器存到每 CPU 暂存区（绝对寻址，不需要暂存寄存器） */
    mov     [g_syscall_scratch + 0],  rax
    mov     [g_syscall_scratch + 8],  rcx
    mov     [g_syscall_scratch + 16], r11
    mov     [g_syscall_scratch + 24], rsp

    /* swapgs 应在此处执行（阶段 3 启用用户态后） */

    /* 切换到内核系统调用栈 */
    mov     rsp, offset g_syscall_stack + (STACK_QWORDS * 8)

    /* 参数重排到 SysV C 约定：RDI=号, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5, 栈=a6 */
    push    r9                      /* a6 作为第 7 个参数压栈 */
    mov     r9, r8                  /* a5 */
    mov     r8, r10                 /* a4 （SysV 的 a4 在 RCX，但内核号占走 RDI，故整体右移） */
    mov     rcx, rdx                /* a3 */
    mov     rdx, rsi                /* a2 */
    mov     rsi, rdi                /* a1 */
    mov     rdi, [g_syscall_scratch + 0]   /* 系统调用号 */

    call    syscall_dispatch        /* 返回值在 RAX */

    add     rsp, 8                  /* 丢弃栈上的 a6 */

    /* 恢复 SYSRET 所需的寄存器 */
    mov     rcx, [g_syscall_scratch + 8]    /* 返回地址 */
    mov     r11, [g_syscall_scratch + 16]   /* RFLAGS */
    mov     rsp, [g_syscall_scratch + 24]   /* 用户栈 */

    /* swapgs 应在此处执行（阶段 3） */
    sysretq

/* 明确声明不需要可执行栈，消除 ld 警告 */
.section .note.GNU-stack,"",@progbits
