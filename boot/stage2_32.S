/*
 * FYXOS stage2 — 进入 64 位长模式
 * ---------------------------------------------------------------------------
 *   1. CR3 = 引导期 PML4
 *   2. CR4.PAE = 1                 （四级页表前置条件）
 *   3. EFER.LME = 1                （MSR 0xC0000080, bit 8）
 *   4. CR0.PG = 1                  （此时 IA-32e 正式生效）
 *   5. far jmp 到 64 位代码段 0x18 -> lm_trampoline
 *
 * 注意：分页必须由客户机代码自己打开。用外部工具直接写 CR0 并不能让
 * 某些 x86 模拟器真正启用 MMU（见 docs/05-testing.md）。
 */
    .intel_syntax noprefix
    .section .text32, "ax"
    .code32
    .globl enter_long_mode

enter_long_mode:
    mov     eax, [esp + 4]        /* pml4 物理地址 */
    mov     cr3, eax

    mov     eax, cr4
    or      eax, 0x00000020       /* PAE */
    mov     cr4, eax

    mov     ecx, 0xC0000080       /* IA32_EFER */
    rdmsr
    or      eax, 0x00000100       /* LME (Long Mode Enable) */
    wrmsr

    mov     eax, cr0
    or      eax, 0x80000001       /* PG | PE */
    mov     cr0, eax

    jmp     0x18, offset lm_trampoline
