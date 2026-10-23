/*
 * FYXOS — x86-64 体系结构接口
 */
#ifndef FYXOS_X86_64_H
#define FYXOS_X86_64_H

#include <fyxos/types.h>

/* ============================== 端口 I/O ================================ */
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
static inline void outw(u16 port, u16 value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}
static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
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
static inline void io_wait(void)
{
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/* ============================ 控制寄存器 / MSR ========================== */
static inline u64 read_cr0(void)
{
    u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline u64 read_cr2(void)
{
    u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static inline u64 read_cr3(void)
{
    u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline u64 read_cr4(void)
{
    u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void write_cr0(u64 v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v)); }
static inline void write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void write_cr4(u64 v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v)); }

#define MSR_EFER        0xC0000080
/* EFER 位定义。EFER.NXE 必须在置任何 PTE 的第 63 位（NX）之前打开，
 * 否则 NX 位属于保留位，访问该页会立刻 #PF（保留位违例）。 */
#define MSR_EFER_SCE    (1ull << 0)     /* SYSCALL/SYSRET 使能 */
#define MSR_EFER_LME    (1ull << 8)     /* 长模式使能 */
#define MSR_EFER_LMA    (1ull << 10)    /* 长模式已激活（只读） */
#define MSR_EFER_NXE    (1ull << 11)    /* No-Execute 使能 */
#define MSR_EFER_SVME   (1ull << 12)    /* SVM 使能 */
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_CSTAR       0xC0000083
#define MSR_SFMASK      0xC0000084
#define MSR_FS_BASE     0xC0000100
#define MSR_GS_BASE     0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 value)
{
    u32 lo = (u32)value, hi = (u32)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static inline void invlpg(void *addr)
{
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static inline void cpu_halt(void) { __asm__ volatile("hlt"); }
static inline void cpu_pause(void) { __asm__ volatile("pause"); }
static inline void enable_interrupts(void) { __asm__ volatile("sti"); }
static inline void disable_interrupts(void) { __asm__ volatile("cli"); }

/* ================================ CPUID ================================= */
struct cpuid_result { u32 eax, ebx, ecx, edx; };
struct cpuid_result cpuid(u32 leaf, u32 subleaf);

struct cpu_features {
    bool has_msr;
    bool has_apic;
    bool has_tsc;
    bool has_syscall;       /* SYSCALL/SYSRET */
    bool has_nx;            /* No-Execute 页保护 */
    bool has_pge;           /* 全局页 */
    bool has_1gb_pages;
    bool has_sse2;
    bool has_avx;
    bool has_rdtscp;
    bool has_invariant_tsc;
    char vendor[13];
    char brand[49];
    u32  family, model, stepping;
    u8   max_physical_bits;
    u8   max_virtual_bits;
};

extern struct cpu_features g_cpu;
void cpu_init(void);

/* ============================ GDT / TSS ================================= */
/* 段选择子（与 arch/x86_64/gdt.c 中的布局严格对应） */
#define GDT_SEL_KERNEL_CODE 0x08
#define GDT_SEL_KERNEL_DATA 0x10
#define GDT_SEL_USER_DATA   0x18
#define GDT_SEL_USER_CODE   0x20
#define GDT_SEL_TSS         0x28

void gdt_init(void);
void gdt_load_tss(void *tss);
void gdt_set_kernel_gs_base(u64 addr);
void gdt_setup_syscall_msrs(void *entry);

/* ============================ IDT / 中断 ================================ */
#define IDT_ENTRIES     256
#define IRQ_BASE        32          /* IRQ0 映射到中断向量 32 */
#define IRQ_TIMER       0
#define IRQ_KEYBOARD    1

typedef void (*irq_handler_t)(void *arg);

/*
 * 中断栈帧布局（必须与 isr.S 中的压栈顺序逐字节对应）：
 *
 *   高地址  SS
 *          RSP
 *          RFLAGS
 *          CS
 *          RIP           <- CPU 自动压入
 *          ERROR_CODE    <- CPU 仅对部分异常压入，否则由 stub 补 0
 *          VECTOR        <- isr stub 压入
 *          RAX ... R15   <- isr_common 压入
 *   低地址  R15
 *
 * 下面按“从低地址到高地址”的顺序声明，即用 frame 指针访问时就是 R15 在前。
 */
struct interrupt_frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 vector;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss;
} PACKED;

void idt_init(void);
void idt_set_gate(u8 vector, void *handler, u8 ist, u8 type_attr);
int  irq_register(u8 irq, irq_handler_t handler, const char *name, void *arg);
void irq_unregister(u8 irq);
void irq_eoi(u8 irq);

void isr_stubs_loaded(void);      /* 占位：确保 isr.S 参与链接 */
void isr_dispatch(struct interrupt_frame *f);

/* ============================== 8259 PIC ================================ */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

void pic_init(void);
void pic_mask(u8 irq);
void pic_unmask(u8 irq);
u16  pic_get_mask(void);
void pic_send_eoi(u8 irq);

/* ============================ 8253/8254 PIT ============================= */
#define PIT_HZ 1193182u

void pit_init(u32 frequency_hz);
u64  pit_ticks(void);           /* 系统启动以来的 tick 数 */
u32  pit_frequency(void);
void pit_stop(void);

/* ============================ 16550 串口 ================================ */
#define COM1_BASE 0x3F8
#define COM2_BASE 0x2F8

void serial_init(u16 port);
int  serial_received(u16 port);
char serial_read(u16 port);
void serial_write(u16 port, char c);
void serial_print(u16 port, const char *s);

/* ============================ VGA 文本模式 ============================== */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_write(const char *s, size_t n);
void vga_set_color(u8 fg, u8 bg);
void vga_get_cursor(u16 *row, u16 *col);
void vga_set_cursor(u16 row, u16 col);

/* ============================== PS/2 键盘 =============================== */
#define KEYBUF_SIZE 256

void keyboard_init(void);
int  keyboard_has_char(void);
char keyboard_getchar(void);          /* 阻塞取一个已解码字符 */
int  keyboard_poll(char *out);        /* 非阻塞，返回 1 表示取到 */
u64  keyboard_irq_count(void);

/* ============================ 调试/关停端口 ============================= */
#define FYX_DEBUG_PORT  0x501      /* QEMU isa-debug-exit 约定端口 */
void NORETURN arch_shutdown(u8 code);
void arch_reboot(void);

#endif /* FYXOS_X86_64_H */
