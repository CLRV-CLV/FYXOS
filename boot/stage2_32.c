/*
 * FYXOS stage2 — 32 位 C 部分
 * ---------------------------------------------------------------------------
 * 运行在 32 位保护模式（平坦段、未开分页），负责：
 *   1. 解析 0x10000 处的 ELF64 内核映像，把 PT_LOAD 搬到 2 MiB 对齐的物理地址
 *   2. 构造引导期四级页表：低端恒等映射 + HHDM 直接映射 + 内核高半核映射
 *   3. 填写 boot_info，然后调用 enter_long_mode() 进入 64 位长模式
 *
 * 这是一个 freestanding 环境：没有 libc、没有栈保护、没有浮点。
 */
#include <fyxos/types.h>
#include <fyxos/boot_info.h>
#include <fyxos/elf.h>
#include "stage2.h"

/* ---- 编译期断言：boot_info 的汇编偏移必须与实际布局一致 ---------------- */
_Static_assert(offsetof(struct fyx_boot_info, magic) == BI_MAGIC, "BI_MAGIC");
_Static_assert(offsetof(struct fyx_boot_info, version) == BI_VERSION, "BI_VERSION");
_Static_assert(offsetof(struct fyx_boot_info, e820_count) == BI_E820_COUNT, "BI_E820_COUNT");
_Static_assert(offsetof(struct fyx_boot_info, boot_drive) == BI_BOOT_DRIVE, "BI_BOOT_DRIVE");
_Static_assert(offsetof(struct fyx_boot_info, kernel_phys) == BI_KERNEL_PHYS, "BI_KERNEL_PHYS");
_Static_assert(offsetof(struct fyx_boot_info, kernel_size) == BI_KERNEL_SIZE, "BI_KERNEL_SIZE");
_Static_assert(offsetof(struct fyx_boot_info, kernel_virt) == BI_KERNEL_VIRT, "BI_KERNEL_VIRT");
_Static_assert(offsetof(struct fyx_boot_info, hhdm_base) == BI_HHDM_BASE, "BI_HHDM_BASE");
_Static_assert(offsetof(struct fyx_boot_info, total_memory) == BI_TOTAL_MEMORY, "BI_TOTAL_MEMORY");
_Static_assert(offsetof(struct fyx_boot_info, framebuffer_addr) == BI_FB_ADDR, "BI_FB_ADDR");
_Static_assert(offsetof(struct fyx_boot_info, framebuffer_pitch) == BI_FB_PITCH, "BI_FB_PITCH");
_Static_assert(offsetof(struct fyx_boot_info, framebuffer_width) == BI_FB_WIDTH, "BI_FB_WIDTH");
_Static_assert(offsetof(struct fyx_boot_info, framebuffer_height) == BI_FB_HEIGHT, "BI_FB_HEIGHT");
_Static_assert(offsetof(struct fyx_boot_info, framebuffer_bpp) == BI_FB_BPP, "BI_FB_BPP");
_Static_assert(offsetof(struct fyx_boot_info, e820) == BI_E820_TABLE, "BI_E820_TABLE");

/* ---- 页表标志 ---------------------------------------------------------- */
#define PT_PRESENT  0x001ull
#define PT_WRITE    0x002ull
#define PT_USER     0x004ull
#define PT_PS       0x080ull        /* 2 MiB 大页（PD 级） */
#define PT_NX       0x8000000000000000ull

/* 内核窗口大小：内核映像 + 引导期临时栈共用 8 MiB */
#define KERNEL_WINDOW_SIZE   (8ull * 1024 * 1024)
#define KERNEL_BOOT_STACK_TOP (KERNEL_VIRT_BASE + KERNEL_WINDOW_SIZE - 16)

/* ---- 与 stage2_16.S / stage2_32.S 交换数据的固定暂存单元 ---------------- */
#define SCRATCH_KERNEL_ENTRY 0x0600ull
#define SCRATCH_STACK_TOP    0x0608ull
#define SCRATCH_BOOT_INFO    0x0610ull

/* 低地址暂存单元：未开分页时 VA==PA，可直接用物理地址访问 */
struct low_scratch {
    volatile u64 kernel_entry;   /* 0x0600 */
    volatile u64 stack_top;      /* 0x0608 */
    volatile u64 boot_info;      /* 0x0610 */
};
#define LOW_SCRATCH ((struct low_scratch *)(uintptr_t)SCRATCH_KERNEL_ENTRY)

#define VGA_BASE   0xB8000u
#define COM1_PORT  0x3F8u

/* ==================== 极简输出：串口 + VGA 文本 ========================= */

static void serial_init(void)
{
    outb(COM1_PORT + 1, 0x00);   /* 关中断 */
    outb(COM1_PORT + 3, 0x80);   /* 打开 DLAB */
    outb(COM1_PORT + 0, 0x01);   /* 115200 波特（除数 1） */
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);   /* 8N1 */
    outb(COM1_PORT + 2, 0xC7);   /* 使能 FIFO */
    outb(COM1_PORT + 4, 0x0B);
}

static int serial_ready(void)
{
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

static void putc(char c)
{
    if (c == '\n') {
        while (!serial_ready()) { }
        outb(COM1_PORT, '\r');
    }
    while (!serial_ready()) { }
    outb(COM1_PORT, (u8)c);

    /* VGA 文本模式（80x25） */
    volatile u16 *vga = (volatile u16 *)(uintptr_t)VGA_BASE;
    static u16 row, col;
    if (c == '\n') {
        row++; col = 0;
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\t') {
        col = (col + 8) & ~7u;
    } else {
        vga[row * 80 + col] = (u16)(0x0F00u | (u8)c);
        col++;
        if (col >= 80) { col = 0; row++; }
    }
    if (row >= 25) { row = 24; }
}

static void puts(const char *s)
{
    while (*s) putc(*s++);
}

static void put_hex(u64 v)
{
    static const char *d = "0123456789ABCDEF";
    puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        putc(d[(v >> i) & 0xF]);
}

static void NORETURN die(const char *msg)
{
    puts("\nFYXOS stage2: FATAL: ");
    puts(msg);
    puts("\n");
    for (;;) { __asm__ volatile("cli; hlt"); }
}

/* ==================== 页表构造 ========================================== */

static u64 pt_pool_next = BOOT_PAGETABLE_ADDR;

static u64 *pt_alloc_page(void)
{
    u64 *page = (u64 *)(uintptr_t)pt_pool_next;
    pt_pool_next += PAGE_SIZE;
    if (pt_pool_next >= BOOT_PAGETABLE_ADDR + 0x10000)
        die("page table pool exhausted");
    for (u32 i = 0; i < PAGE_SIZE / 8; i++)
        page[i] = 0;
    return page;
}

static u64 *pt_next_level(u64 *table, u32 index, u64 flags)
{
    u64 entry = table[index];
    u64 *next;
    if (entry & PT_PRESENT)
        return (u64 *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ull);
    next = pt_alloc_page();
    table[index] = ((u64)(uintptr_t)next & 0x000FFFFFFFFFF000ull) | flags;
    return next;
}

/* 以 2 MiB 大页映射一段地址 */
static void map_2mb(u64 *pml4, u64 virt, u64 phys, u64 flags)
{
    u64 *pdpt = pt_next_level(pml4, (virt >> 39) & 0x1FF, PT_PRESENT | PT_WRITE);
    u64 *pd   = pt_next_level(pdpt, (virt >> 30) & 0x1FF, PT_PRESENT | PT_WRITE);
    pd[(virt >> 21) & 0x1FF] =
        (phys & 0x000FFFFFFFE00000ull) | PT_PS | PT_PRESENT | flags;
}

static void map_range_2mb(u64 *pml4, u64 virt, u64 phys, u64 size, u64 flags)
{
    for (u64 off = 0; off < size; off += BIGPAGE_SIZE)
        map_2mb(pml4, virt + off, phys + off, flags);
}

/* ==================== ELF 加载 ========================================== */

static struct elf64_ehdr *kernel_elf(void)
{
    struct elf64_ehdr *eh = (struct elf64_ehdr *)(uintptr_t)KERNEL_STAGE_ADDR;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        die("kernel image is not an ELF");
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB)
        die("kernel is not little-endian ELF64");
    if (eh->e_machine != EM_X86_64)
        die("kernel is not x86-64");
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        die("unsupported ELF type");
    return eh;
}

int stage2_main(void)
{
    serial_init();
    puts("FYXOS stage2 (32-bit)\n");

    struct fyx_boot_info *bi =
        (struct fyx_boot_info *)(uintptr_t)BOOT_INFO_ADDR;
    if (bi->magic != FYX_BOOT_MAGIC)
        die("bad boot_info magic");

    puts("  memory map: ");
    put_hex(bi->total_memory);
    puts(" bytes usable in ");
    put_hex(bi->e820_count);
    puts(" regions\n");

    /* ---------------- 1. 加载 ELF 段 ------------------------------------ */
    struct elf64_ehdr *eh = kernel_elf();
    struct elf64_phdr *ph =
        (struct elf64_phdr *)(uintptr_t)(KERNEL_STAGE_ADDR + eh->e_phoff);

    u64 phys_end = 0;
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        u8 *dst = (u8 *)(uintptr_t)ph[i].p_paddr;
        const u8 *src =
            (const u8 *)(uintptr_t)(KERNEL_STAGE_ADDR + ph[i].p_offset);
        for (u64 n = 0; n < ph[i].p_filesz; n++)
            dst[n] = src[n];
        for (u64 n = ph[i].p_filesz; n < ph[i].p_memsz; n++)
            dst[n] = 0;
        if (ph[i].p_paddr + ph[i].p_memsz > phys_end)
            phys_end = ph[i].p_paddr + ph[i].p_memsz;
    }

    u64 kernel_phys = KERNEL_PHYS_BASE;
    puts("  kernel: phys ");
    put_hex(kernel_phys);
    puts(" -> ");
    put_hex(phys_end);
    puts("  entry ");
    put_hex(eh->e_entry);
    puts("\n");

    /* ---------------- 2. 构造页表 --------------------------------------- */
    u64 *pml4 = pt_alloc_page();

    /* 低端 4 GiB 恒等映射（引导期代码、VGA、ACPI 等仍需物理地址访问） */
    map_range_2mb(pml4, 0, 0, 4ull * 1024 * 1024 * 1024, PT_WRITE);
    /* 物理内存直接映射窗口 HHDM */
    map_range_2mb(pml4, HHDM_BASE, 0, 4ull * 1024 * 1024 * 1024, PT_WRITE);
    /* 内核高半核窗口（含临时引导栈） */
    map_range_2mb(pml4, KERNEL_VIRT_BASE, KERNEL_PHYS_BASE,
                  KERNEL_WINDOW_SIZE, PT_WRITE);

    puts("  page tables built at ");
    put_hex(BOOT_PAGETABLE_ADDR);
    puts("\n");

    /* ---------------- 3. 填写 boot_info --------------------------------- */
    bi->kernel_phys  = kernel_phys;
    bi->kernel_size  = phys_end - kernel_phys;
    bi->kernel_virt  = KERNEL_VIRT_BASE;
    bi->hhdm_base    = HHDM_BASE;

    /* ---------------- 4. 交接给 64 位跳板 -------------------------------- */
    /* 访问固定物理地址必然触发 -Warray-bounds，这里是有意为之 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    LOW_SCRATCH->kernel_entry = eh->e_entry;
    LOW_SCRATCH->stack_top    = KERNEL_BOOT_STACK_TOP;
    LOW_SCRATCH->boot_info    = HHDM_BASE + BOOT_INFO_ADDR;
#pragma GCC diagnostic pop

    puts("  entering long mode...\n");
    enter_long_mode((u32)(uintptr_t)pml4);
    die("enter_long_mode returned");
}
