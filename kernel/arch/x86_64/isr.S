/*
 * FYXOS — 中断/异常入口（ISR stubs）
 * ---------------------------------------------------------------------------
 * 用 GAS 宏生成全部 256 个入口。带错误码的异常（CPU 已自动压入）不再补 0，
 * 其余统一补一个 0，使所有栈帧结构一致。
 */
    .intel_syntax noprefix
    .section .text, "ax"
    .code64
    .altmacro

/* 带错误码的异常向量 */
#define HAS_ERR(v) ((v)==8 || (v)==10 || (v)==11 || (v)==12 || (v)==13 || \
                    (v)==14 || (v)==17 || (v)==21 || (v)==29 || (v)==30)

.macro GEN_STUB n
    .globl isr_\n
    .type isr_\n, @function
isr_\n:
    .if HAS_ERR(\n)
        push    \n                  /* 只压向量号，错误码由 CPU 压好 */
    .else
        push    0                   /* 占位错误码 */
        push    \n
    .endif
    jmp     isr_common
.endm

.set vec, 0
.rept 256
    GEN_STUB %vec
    .set vec, vec + 1
.endr

/* -------------------------------------------------------------------------- */
/*
 * 桩地址表（C 侧 idt_init 直接读它来填 IDT）。
 * 说明：本版本的 GNU as 在 `.rept` 里不支持 altmacro 的 %expr 求值
 *      （`.quad isr_%i` 会被解析成取模），而宏体内的 \n 可以正常替换，
 *      因此这里改用 .irp 显式展开 0..47，其余向量指向 isr_spurious。
 */
    .globl isr_stub_table
    .globl isr_spurious
    .section .rodata, "a"
    .align 16
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr_\n
.endr
.rept 81                        /* 48..128 */
    .quad isr_spurious
.endr
    .quad isr_129                /* 调度软中断 */
.rept 126                        /* 130..255 */
    .quad isr_spurious
.endr

/* 未使用的向量统一落到这里：发 EOI 后直接返回，避免误判为致命异常 */
    .section .text, "ax"
    .code64
    .globl isr_spurious
isr_spurious:
    push    0
    push    255
    jmp     isr_common

/* -------------------------------------------------------------------------- */
    .section .text, "ax"
    .code64
    .globl isr_common
    .extern isr_dispatch

isr_common:
    /* 依次压入通用寄存器（顺序须与 struct interrupt_frame 一致） */
    push    rax
    push    rcx
    push    rdx
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    /* 内核态到内核态的中断：不需要 swapgs（用户态进入时再加） */
    mov     rdi, rsp                /* 参数 0 = 栈帧指针 */
    cld
    call    isr_dispatch
    .globl isr_return_point
isr_return_point:

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    pop     rdx
    pop     rcx
    pop     rax

    add     rsp, 16                 /* 跳过 vector + error_code */
    iretq

/* GDT / TSS 装载辅助 ------------------------------------------------------- */
    .globl gdt_flush
gdt_flush:
    /* RDI = struct gdtr 的地址。
     * CS 无需远跳转刷新：新 GDT 在 0x08 处放置的同样是 64 位代码段，
     * 与 stage2 建立的选择子完全兼容。 */
    lgdt    [rdi]
    mov     ax, 0x10                /* GDT_SEL_KERNEL_DATA */
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ret

    .globl tss_flush
tss_flush:
    ltr     di                      /* RDI = TSS 选择子 */
    ret

/* 明确声明不需要可执行栈，消除 ld 警告 */
.section .note.GNU-stack,"",@progbits
