/*
 * FYXOS — 8259 可编程中断控制器
 * 把主/从 PIC 的 IRQ0..IRQ15 重映射到中断向量 0x20..0x2F，
 * 避开 CPU 异常向量 0x00..0x1F。
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

static u16 g_mask = 0xFFFF;      /* 初始屏蔽全部 IRQ */

void pic_init(void)
{
    /* 保存（我们直接重映射，不关心 BIOS 的旧掩码） */
    g_mask = 0xFFFF;

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, IRQ_BASE);            /* 主片 IRQ0 -> 向量 0x20 */
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);        /* 从片 IRQ8 -> 向量 0x28 */
    io_wait();

    outb(PIC1_DATA, 0x04);                /* 主片 IR2 上挂从片 */
    io_wait();
    outb(PIC2_DATA, 0x02);                /* 从片的级联标识 */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* 屏蔽全部，由各驱动按需 unmask */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(u8 irq)
{
    u16 port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    g_mask |= (u16)(1u << irq);
    outb(port, (u8)(g_mask >> (port == PIC1_DATA ? 0 : 8)));
}

void pic_unmask(u8 irq)
{
    u16 port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    g_mask &= (u16)~(1u << irq);
    outb(port, (u8)(g_mask >> (port == PIC1_DATA ? 0 : 8)));
}

u16 pic_get_mask(void)
{
    return g_mask;
}

void pic_send_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}
