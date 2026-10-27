/*
 * FYXOS — 中断描述符表（IDT）与 IRQ 分发
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>

#define IDT_TYPE_INTERRUPT 0x8E    /* 中断门：present, DPL0, 类型 0xE */
#define IDT_TYPE_TRAP      0x8F

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} PACKED;

struct idtr {
    u16 limit;
    u64 base;
} PACKED;

struct irq_binding {
    irq_handler_t handler;
    const char   *name;
    void         *arg;
    u64           count;
};

/* isr.S 生成的 256 个入口地址表 */
extern u64 isr_stub_table[IDT_ENTRIES];

static struct idt_entry  g_idt[IDT_ENTRIES];
static struct idtr       g_idtr;
static struct irq_binding g_irq[16];
static bool              g_ready;

static const char *const exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection",
    "Page Fault", "Reserved", "x87 FPU Error", "Alignment Check",
    "Machine Check", "SIMD FPU Exception", "Virtualization",
    "Control Protection", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security Exception", "Reserved"
};

void idt_set_gate(u8 vector, void *handler, u8 ist, u8 type_attr)
{
    u64 addr = (u64)(uintptr_t)handler;
    struct idt_entry *e = &g_idt[vector];

    e->offset_low  = (u16)(addr & 0xFFFF);
    e->selector    = GDT_SEL_KERNEL_CODE;
    e->ist         = ist & 0x7;
    e->type_attr   = type_attr;
    e->offset_mid  = (u16)((addr >> 16) & 0xFFFF);
    e->offset_high = (u32)((addr >> 32) & 0xFFFFFFFF);
    e->zero        = 0;
}

void idt_init(void)
{
    memset(g_idt, 0, sizeof(g_idt));
    memset(g_irq, 0, sizeof(g_irq));

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate((u8)i, (void *)isr_stub_table[i], 0, IDT_TYPE_INTERRUPT);

    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base  = (u64)(uintptr_t)&g_idt;

    __asm__ volatile("lidt %0" :: "m"(g_idtr));
    g_ready = true;
}

int irq_register(u8 irq, irq_handler_t handler, const char *name, void *arg)
{
    if (irq >= 16 || !handler)
        return -FYX_EINVAL;
    if (g_irq[irq].handler)
        return -FYX_EAGAIN;

    g_irq[irq].handler = handler;
    g_irq[irq].name = name ? name : "irq";
    g_irq[irq].arg = arg;
    g_irq[irq].count = 0;
    return 0;
}

void irq_unregister(u8 irq)
{
    if (irq < 16)
        memset(&g_irq[irq], 0, sizeof(g_irq[irq]));
}

void irq_eoi(u8 irq)
{
    pic_send_eoi(irq);
}

static void describe_page_fault(u64 error_code)
{
    kprintf("  PF: %s %s %s %s %s\n",
            (error_code & 0x01) ? "protection-violation" : "not-present",
            (error_code & 0x02) ? "write" : "read",
            (error_code & 0x04) ? "user" : "supervisor",
            (error_code & 0x08) ? "reserved-bit" : "",
            (error_code & 0x10) ? "instruction-fetch" : "");
    kprintf("  PF: faulting address = 0x%016llx\n",
            (unsigned long long)read_cr2());
}

extern void sched_switch(void);
#define SCHED_YIELD_VECTOR 0x81

void isr_dispatch(struct interrupt_frame *f)
{
    /* 调度软中断：thread_yield() 用 int $0x81 进入，
     * 让切换同样发生在中断返回路径上，保持“每个线程栈上都有一份完整帧”的不变式。*/
    if (f->vector == SCHED_YIELD_VECTOR) {
        sched_switch();
        return;
    }

    if (f->vector >= IRQ_BASE && f->vector < IRQ_BASE + 16) {
        u8 irq = (u8)(f->vector - IRQ_BASE);
        struct irq_binding *b = &g_irq[irq];
        if (b->handler) {
            b->count++;
            b->handler(b->arg);
        } else {
            /* 无人认领的 IRQ 必须发 EOI，否则后续中断全部被屏蔽 */
            pic_send_eoi(irq);
        }
        return;
    }

    if (f->vector < 32) {
        const char *name = exception_names[f->vector];
        disable_interrupts();
        console_set_color(0x4, 0x0);
        kprintf("\n*** EXCEPTION: %s (vector %u, error 0x%llx) ***\n",
                name, (unsigned)f->vector,
                (unsigned long long)f->error_code);
        kprintf("  RIP=0x%016llx  CS=0x%04llx  RFLAGS=0x%016llx\n",
                (unsigned long long)f->rip, (unsigned long long)f->cs,
                (unsigned long long)f->rflags);
        kprintf("  RSP=0x%016llx  SS=0x%04llx\n",
                (unsigned long long)f->rsp, (unsigned long long)f->ss);
        if (f->vector == 14)
            describe_page_fault(f->error_code);
        console_set_color(0x7, 0x0);
        arch_shutdown(0xFE);        /* 0xFE = 因异常停机 */
    }

    /* 未知向量：直接 EOI 并返回 */
    pic_send_eoi(0);
}

void isr_stubs_loaded(void)
{
    /* 仅用于确保 isr.S 的目标文件参与链接 */
}
