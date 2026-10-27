/*
 * FYXOS — 上下文切换（汇编部分）
 * ---------------------------------------------------------------------------
 * 设计要点：**只在中断上下文里切换线程**。
 *
 * 每个线程被换出时，栈上都保存着一份完整的中断栈帧（isr_common 压入的
 * 通用寄存器 + CPU 压入的 RIP/CS/RFLAGS/RSP/SS）。切换时只需更换 RSP，
 * 随后由 isr_common 恢复寄存器、iretq 落回新线程被中断的地方。
 *
 * 这样“阻塞/退出”可以简化为：改状态 + sti/hlt —— 定时器中断一来自然切走。
 */
    .intel_syntax noprefix
    .section .text, "ax"
    .code64

/*
 * void irq_context_switch(uintptr_t *prev_rsp_slot, uintptr_t next_rsp);
 *   RDI = &prev->rsp    保存当前 RSP
 *   RSI = next->rsp     载入新线程的 RSP
 *
 * 必须是裸函数：不能有任何 prologue，否则保存的 RSP 与 isr_common 的
 * 约定会错位。
 */
    .globl irq_context_switch
irq_context_switch:
    mov     [rdi], rsp
    mov     rsp, rsi
    ret

/*
 * 新线程的“出厂”返回栈：
 *   irq_context_switch 的 ret 会跳到这里，这里再 ret 就进入 isr_common
 *   的返回点，接着由 isr_common 弹出中断帧并 iretq 到线程入口。
 */
    .globl isr_dispatch_return_stub
isr_dispatch_return_stub:
    ret

/*
 * 线程入口跳板：中断帧里已把 rbx 设为线程函数、rdi 设为参数。
 * 函数返回时把返回值作为退出码传给 thread_exit。
 */
    .globl thread_trampoline
    .extern thread_exit
thread_trampoline:
    call    rbx
    mov     rdi, rax              /* 返回值作为退出码 */
    call    thread_exit
    /* thread_exit 不返回；万一返回就停机 */
1:
    cli
    hlt
    jmp     1b

/* 明确声明不需要可执行栈，消除 ld 警告 */
.section .note.GNU-stack,"",@progbits
