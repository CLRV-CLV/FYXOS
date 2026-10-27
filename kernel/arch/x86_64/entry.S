/*
 * FYXOS 内核入口（64 位长模式）
 * ---------------------------------------------------------------------------
 * stage2 的 lm_trampoline 跳转到这里时：
 *   CS  = 64 位代码段
 *   RSP = 引导期临时栈（stage2 在内核窗口内预留）
 *   RDI = boot_info 的**虚拟**地址（HHDM + 0x5000）
 *
 * 本段只做最小必要的工作：换用内核自己的引导栈、清标志、把 RDI 作为第一个
 * 参数传給 kmain(struct fyx_boot_info *) —— 与 SysV ABI 一致。
 */
    .intel_syntax noprefix
    .section .text.entry, "ax"
    .code64
    .globl _kernel_entry
    .extern kmain

_kernel_entry:
    cli
    cld

    /* 换用内核自己的引导栈（gdt/sched 初始化后会切换到各线程的内核栈） */
    mov     rsp, offset _boot_stack_top
    xor     rbp, rbp

    /* 清掉 stage2 遗留的段寄存器状态；数据段已在 trampoline 里设好 */
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    /* RDI = boot_info，已是 kmain 的第一个参数 */
    xor     rsi, rsi
    xor     rdx, rdx
    call    kmain

    /* kmain 不应返回；若返回则停机 */
    cli
1:
    hlt
    jmp     1b

/* 明确声明不需要可执行栈，消除 ld 警告 */
.section .note.GNU-stack,"",@progbits
