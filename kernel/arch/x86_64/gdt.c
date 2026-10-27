/*
 * FYXOS — 全局描述符表（GDT）与任务状态段（TSS）
 * ---------------------------------------------------------------------------
 * 布局（与 include/fyxos/x86_64.h 中的选择子常量严格对应）：
 *   0x00  null
 *   0x08  内核代码段（64 位, DPL0）
 *   0x10  内核数据段（DPL0）
 *   0x18  用户数据段（DPL3）
 *   0x20  用户代码段（64 位, DPL3）
 *   0x28  TSS（16 字节，占两个表项）
 *
 * 同时为 SYSCALL/SYSRET 与 swapgs 准备好各段选择子（STAR/LSTAR）。
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>

#define GDT_ENTRIES 7

/* 访问权限字节 */
#define SEG_PRESENT   0x80
#define SEG_DPL0      0x00
#define SEG_DPL3      0x60
#define SEG_CODE      0x18     /* code,非 conforming,可读 */
#define SEG_DATA      0x12     /* data,向上扩展,可写 */
#define SEG_TSS       0x09     /* 64 位 TSS */
#define SEG_LONG      0x20     /* L 位：64 位代码段 */

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  limit_high_flags;
    u8  base_high;
} PACKED;

struct tss_entry {
    u32 reserved0;
    u64 rsp0;          /*  Ring 0 栈指针 */
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;
} PACKED;

struct gdtr {
    u16 limit;
    u64 base;
} PACKED;

static struct gdt_entry g_gdt[GDT_ENTRIES];
static struct gdtr      g_gdtr;
static struct tss_entry g_tss;

extern void gdt_flush(uintptr_t gdtr_addr);
extern void tss_flush(u16 selector);

static void gdt_set(int index, u32 base, u32 limit, u8 access, u8 flags)
{
    g_gdt[index].limit_low  = (u16)(limit & 0xFFFF);
    g_gdt[index].base_low   = (u16)(base & 0xFFFF);
    g_gdt[index].base_mid   = (u8)((base >> 16) & 0xFF);
    g_gdt[index].access     = access | SEG_PRESENT;
    g_gdt[index].limit_high_flags =
        (u8)(((limit >> 16) & 0x0F) | ((flags & 0x0F) << 4));
    g_gdt[index].base_high  = (u8)((base >> 24) & 0xFF);
}

/* 64 位 TSS 描述符占 16 字节，需要手工填写第二个表项 */
static void gdt_set_tss(int index, uintptr_t base, u32 limit)
{
    struct gdt_entry *e = &g_gdt[index];
    e->limit_low  = (u16)(limit & 0xFFFF);
    e->base_low   = (u16)(base & 0xFFFF);
    e->base_mid   = (u8)((base >> 16) & 0xFF);
    e->access     = SEG_TSS | SEG_PRESENT;
    e->limit_high_flags = (u8)(((limit >> 16) & 0x0F) | 0x00);
    e->base_high  = (u8)((base >> 24) & 0xFF);

    struct gdt_entry *hi = &g_gdt[index + 1];
    hi->limit_low = (u16)((base >> 32) & 0xFFFF);
    hi->base_low  = (u16)((base >> 48) & 0xFFFF);
    hi->base_mid  = 0;
    hi->access    = 0;
    hi->limit_high_flags = 0;
    hi->base_high = 0;
}

/*
 * 用 far ret 重新装载 CS：把目标 CS 与 RIP 依次压栈，再 lretq。
 * 不能只用 mov 写 CS，x86 没有这个指令。
 */
static inline void reload_cs(void)
{
    __asm__ volatile(
        "pushq  $0x08\n\t"                 /* CS = 内核代码段 */
        "leaq   1f(%%rip), %%rax\n\t"
        "pushq  %%rax\n\t"                 /* RIP = 下面的标号 1 */
        "lretq\n\t"
        "1:\n\t"
        : : : "rax", "memory");
}

void gdt_init(void)
{
    memset(g_gdt, 0, sizeof(g_gdt));

    /* flags: 高 4 位为 limit[19:16]，低 4 位中 bit3=粒度 bit2=L/D */
    gdt_set(1, 0, 0xFFFFF, SEG_CODE | SEG_DPL0, 0xA);   /* 0x08 内核代码 */
    gdt_set(2, 0, 0xFFFFF, SEG_DATA | SEG_DPL0, 0xA);   /* 0x10 内核数据 */
    gdt_set(3, 0, 0xFFFFF, SEG_DATA | SEG_DPL3, 0xA);   /* 0x18 用户数据 */
    gdt_set(4, 0, 0xFFFFF, SEG_CODE | SEG_DPL3, 0xA);   /* 0x20 用户代码 */

    /* 用户代码段必须是 64 位段（L=1）：0xA0 中的 0x2 位即 L 位 */
    g_gdt[4].limit_high_flags |= 0x20;

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.iopb_offset = sizeof(struct tss_entry);
    gdt_set_tss(5, (uintptr_t)&g_tss, sizeof(struct tss_entry) - 1);

    g_gdtr.limit = (u16)(sizeof(g_gdt) - 1);
    g_gdtr.base  = (u64)(uintptr_t)&g_gdt;

    gdt_flush((uintptr_t)&g_gdtr);

    /*
     * 必须用远跳转（这里是 far ret 形式）重新装载 CS。
     *
     * lgdt 只改变 GDTR，CS 里仍然装着引导器留下的选择子 —— stage2 的 GDT 里
     * 0x18 是 64 位代码段，而内核 GDT 的 0x18 是**用户数据段**。不重载的话，
     * 之后任何把 CS 压栈再弹出的路径（iretq/sysret）都会去加载一个数据段
     * 当代码段，直接 #GP。真机同样会挂。
     */
    reload_cs();
    tss_flush(GDT_SEL_TSS);
}

/*
 * 更新 TSS 里的 RSP0。
 *
 * ring 3 执行时来了中断/异常，CPU 会无条件从 TSS 取 RSP0 当作内核栈。
 * 这个值必须随任务走，否则用户进程一被时钟打断就会在没有映射的栈上压帧。
 * 内核线程不走这条路（它们本来就在 ring 0，不切换栈），但每次切换任务时
 * 一并更新也没有坏处。
 */
void gdt_set_kernel_stack(u64 rsp0)
{
    g_tss.rsp0 = rsp0;
}

void gdt_load_tss(void *tss)
{
    UNUSED(tss);
    tss_flush(GDT_SEL_TSS);
}

/* 为每个 CPU 设置 GS 基址（内核态 per-cpu 数据用 swapgs 访问） */
void gdt_set_kernel_gs_base(u64 addr)
{
    wrmsr(MSR_KERNEL_GS_BASE, addr);
    wrmsr(MSR_GS_BASE, addr);
}

/* 供系统调用入口使用：设置 SYSCALL 的目标与段选择子 */
void gdt_setup_syscall_msrs(void *entry)
{
    /* STAR[63:48] = 用户代码段选择子（sysret 用）
     * STAR[47:32] = 内核代码段选择子（syscall 用） */
    /*
     * STAR[63:48] 不是直接使用的选择子，sysret 会拿它做算术：
     *     CS = STAR[63:48] + 16
     *     SS = STAR[63:48] + 8
     * 我们的 GDT 里用户代码在 0x20、用户数据在 0x18，带 RPL3 分别是
     * 0x23 与 0x1b，两者正好相差 8。于是 STAR[63:48] 必须填 0x13：
     *     0x13 + 16 = 0x23 (用户代码)
     *     0x13 +  8 = 0x1b (用户数据)
     * 早先这里填的是 GDT_SEL_USER_CODE(0x20)，sysret 会算出 CS=0x30 —— 一个
     * 不存在的描述符，用户态返回时直接 #GP。没有用户态时这个错误不会被触发，
     * 所以一直潜伏到阶段 G 才暴露。
     */
    u64 star = ((u64)GDT_STAR_USER_BASE << 48) | ((u64)GDT_SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (u64)(uintptr_t)entry);
    wrmsr(MSR_CSTAR, 0);          /* 32 位兼容模式不提供支持 */
    wrmsr(MSR_SFMASK, 0x200);     /* syscall 时清 IF，进入时关中断 */
}
