/*
 * FYXOS stage2 — 32 位 freestanding 环境的内部接口
 */
#ifndef FYXOS_STAGE2_H
#define FYXOS_STAGE2_H

#include <fyxos/types.h>

/* 端口 I/O（编译为内联汇编，freestanding） */
static inline void outb(u16 port, u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(u16 port, u32 value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port)
{
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* 由 stage2_32.S 提供：装载 CR3/CR4/EFER/CR0 并 far jump 到 64 位跳板 */
void NORETURN enter_long_mode(u32 pml4_phys);

/* 由 stage2_32.c 提供，被 pm_entry 调用 */
int stage2_main(void);

#endif /* FYXOS_STAGE2_H */
