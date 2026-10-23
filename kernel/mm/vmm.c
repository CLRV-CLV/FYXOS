/*
 * FYXOS — 虚拟内存管理（x86-64 四级页表）
 * ---------------------------------------------------------------------------
 * 地址空间布局：
 *   0x0000000000000000 .. 0x00007FFFFFFFFFFF   用户空间（阶段 3 启用进程）
 *   0xFFFF800000000000 ..                      物理内存直接映射窗口（HHDM）
 *   0xFFFFFFFF80000000 ..                      内核映像
 *   0xFFFFFFFF90000000 ..                      内核堆
 *
 * vmm_init() 会**完全重建**页表，丢掉 stage2 的引导页表：
 *   - HHDM 覆盖全部探测到的物理内存
 *   - 内核各段按节区属性分别设置 R/W/X/NX
 *   - 不再保留低端恒等映射（所有物理访问统一走 HHDM）
 */
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/x86_64.h>
#include <fyxos/sync.h>

#define PTE_PRESENT  0x001ull
#define PTE_WRITE    0x002ull
#define PTE_USER     0x004ull
#define PTE_PWT      0x008ull
#define PTE_PCD      0x010ull
#define PTE_ACCESSED 0x020ull
#define PTE_DIRTY    0x040ull
#define PTE_PS       0x080ull
#define PTE_GLOBAL   0x100ull
#define PTE_NX       0x8000000000000000ull

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull
#define INDEX(v, shift) (((v) >> (shift)) & 0x1FF)

extern u8 _text_start[], _text_end[];
extern u8 _rodata_start[], _rodata_end[];
extern u8 _data_start[], _data_end[];
extern u8 _bss_start[], _bss_end[];
extern u8 _boot_stack_bottom[], _boot_stack_top[];
extern u8 _kernel_end[];
extern u8 _kernel_phys_end[];

static phys_addr_t g_kernel_pml4;
static spinlock_t  g_vmm_lock = SPINLOCK_INIT("vmm");
static u64         g_map_calls, g_unmap_calls, g_faults_tables;

static inline u64 *table_virt(u64 entry)
{
    return (u64 *)phys_to_virt(entry & PTE_ADDR_MASK);
}

static u64 *table_get_or_create(u64 *table, u32 index, u64 flags)
{
    if (table[index] & PTE_PRESENT)
        return table_virt(table[index]);

    phys_addr_t page = pmm_alloc_frame();
    if (page == PMM_INVALID)
        return NULL;

    u64 *virt = (u64 *)phys_to_virt(page);
    memset(virt, 0, PAGE_SIZE);
    table[index] = (page & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | flags;
    g_faults_tables++;
    return virt;
}

static u64 translate_flags(u32 flags)
{
    u64 pte = PTE_PRESENT;
    if (flags & VMM_WRITE)
        pte |= PTE_WRITE;
    if (flags & VMM_USER)
        pte |= PTE_USER;
    if (flags & VMM_NOCACHE)
        pte |= PTE_PCD | PTE_PWT;
    if ((flags & VMM_NOEXEC) && g_cpu.has_nx)
        pte |= PTE_NX;
    return pte;
}

/* 以 4 KiB 页映射一段地址 */
/* root 是目标 PML4 的内核虚拟地址：内核空间与用户进程空间共用这套走表逻辑 */
static int map_4k_root(u64 *pml4, u64 virt, phys_addr_t phys, size_t size, u32 flags)
{
    u64 pte_flags = translate_flags(flags);

    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        u64 v = virt + off;
        phys_addr_t p = phys + off;

        u64 *pdpt = table_get_or_create(pml4, INDEX(v, 39), 0);
        if (!pdpt)
            return -1;
        u64 *pd = table_get_or_create(pdpt, INDEX(v, 30), 0);
        if (!pd)
            return -1;
        u64 *pt = table_get_or_create(pd, INDEX(v, 21), 0);
        if (!pt)
            return -1;

        pt[INDEX(v, 12)] = (p & PTE_ADDR_MASK) | pte_flags;
        invlpg((void *)v);
    }
    return 0;
}

static int map_4k(u64 virt, phys_addr_t phys, size_t size, u32 flags)
{
    return map_4k_root((u64 *)phys_to_virt(g_kernel_pml4), virt, phys, size, flags);
}

/* 以 2 MiB 大页映射一段地址：用于 HHDM 这类大块、连续的映射 */
static int map_2m(u64 virt, phys_addr_t phys, size_t size, u32 flags)
{
    u64 pte_flags = translate_flags(flags) | PTE_PS;
    u64 *pml4 = (u64 *)phys_to_virt(g_kernel_pml4);

    for (size_t off = 0; off < size; off += BIGPAGE_SIZE) {
        u64 v = virt + off;
        phys_addr_t p = phys + off;

        u64 *pdpt = table_get_or_create(pml4, INDEX(v, 39), 0);
        if (!pdpt)
            return -1;
        u64 *pd = table_get_or_create(pdpt, INDEX(v, 30), 0);
        if (!pd)
            return -1;

        pd[INDEX(v, 21)] = (p & 0x000FFFFFFFE00000ull) | pte_flags;
        invlpg((void *)v);
    }
    return 0;
}

void vmm_switch(phys_addr_t pml4_phys)
{
    write_cr3(pml4_phys & PTE_ADDR_MASK);
}

phys_addr_t vmm_current_pml4(void)
{
    return (phys_addr_t)(read_cr3() & PTE_ADDR_MASK);
}

phys_addr_t vmm_kernel_pml4(void)
{
    return g_kernel_pml4;
}

void vmm_init(struct fyx_boot_info *bi)
{
    phys_addr_t pml4 = pmm_alloc_frame();
    if (pml4 == PMM_INVALID)
        panic("vmm: cannot allocate PML4");
    memset(phys_to_virt(pml4), 0, PAGE_SIZE);
    g_kernel_pml4 = pml4;

    /* 1) HHDM：覆盖全部物理内存（向上取整到 2 MiB） */
    u64 ram_bytes = 0;
    for (u32 i = 0; i < bi->e820_count && i < FYX_E820_MAX; i++)
        if (bi->e820[i].type == FYX_MEM_USABLE)
            ram_bytes += bi->e820[i].length;
    u64 hhdm_size = ALIGN_UP(ram_bytes + (64ull << 20), BIGPAGE_SIZE);
    if (hhdm_size < (256ull << 20))
        hhdm_size = 256ull << 20;

    if (map_2m(HHDM_BASE, 0, hhdm_size, VMM_WRITE | VMM_NOEXEC) != 0)
        panic("vmm: HHDM mapping failed");

    /* 2) 内核映像：按节区分别设置权限 */
    u64 kbase = KERNEL_VIRT_BASE;
    u64 kphys = 0x200000;      /* 与 linker.ld 的 KERNEL_LMA 一致 */

    struct { u64 vs, ve; u32 flags; } segs[] = {
        { (u64)_text_start,   (u64)_text_end,   0 },
        { (u64)_rodata_start, (u64)_rodata_end, VMM_NOEXEC },
        { (u64)_data_start,   (u64)_data_end,   VMM_WRITE | VMM_NOEXEC },
        { (u64)_bss_start,    (u64)_bss_end,   VMM_WRITE | VMM_NOEXEC },
        /* 引导栈是 NOLOAD 节，不落在这四个节区之内。早期内核用的就是它，
         * 必须一并映射，否则 mov cr3 之后第一次压栈/出栈就会缺页。 */
        { (u64)_boot_stack_bottom, (u64)_boot_stack_top,
                                       VMM_WRITE | VMM_NOEXEC },
    };

    for (size_t i = 0; i < ARRAY_SIZE(segs); i++) {
        if (segs[i].ve <= segs[i].vs)
            continue;
        size_t size = ALIGN_UP(segs[i].ve - segs[i].vs, PAGE_SIZE);
        u64 p = kphys + (segs[i].vs - kbase);
        if (map_4k(ALIGN_DOWN(segs[i].vs, PAGE_SIZE),
                   ALIGN_DOWN(p, PAGE_SIZE), size, segs[i].flags) != 0)
            panic("vmm: kernel section mapping failed");
    }

    /* 3) 切换页表：此后低端恒等映射失效 */
    vmm_switch(pml4);
    pmm_lift_early_limit();
}

int vmm_map(u64 virt, phys_addr_t phys, size_t size, u32 flags)
{
    if (size == 0)
        return -FYX_EINVAL;
    spin_lock(&g_vmm_lock);
    int r = map_4k(ALIGN_DOWN(virt, PAGE_SIZE), ALIGN_DOWN(phys, PAGE_SIZE),
                   ALIGN_UP(size, PAGE_SIZE), flags);
    g_map_calls++;
    spin_unlock(&g_vmm_lock);
    return r;
}

int vmm_unmap(u64 virt, size_t size)
{
    if (size == 0)
        return -FYX_EINVAL;
    spin_lock(&g_vmm_lock);

    u64 *pml4 = (u64 *)phys_to_virt(g_kernel_pml4);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        u64 v = ALIGN_DOWN(virt + off, PAGE_SIZE);
        if (!(pml4[INDEX(v, 39)] & PTE_PRESENT))
            continue;
        u64 *pdpt = table_virt(pml4[INDEX(v, 39)]);
        if (!(pdpt[INDEX(v, 30)] & PTE_PRESENT))
            continue;
        u64 *pd = table_virt(pdpt[INDEX(v, 30)]);
        if (!(pd[INDEX(v, 21)] & PTE_PRESENT) || (pd[INDEX(v, 21)] & PTE_PS))
            continue;
        u64 *pt = table_virt(pd[INDEX(v, 21)]);
        pt[INDEX(v, 12)] = 0;
        invlpg((void *)v);
    }
    g_unmap_calls++;
    spin_unlock(&g_vmm_lock);
    return 0;
}

int vmm_protect(u64 virt, size_t size, u32 flags)
{
    if (size == 0)
        return -FYX_EINVAL;
    spin_lock(&g_vmm_lock);

    u64 *pml4 = (u64 *)phys_to_virt(g_kernel_pml4);
    u64 want = translate_flags(flags);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        u64 v = ALIGN_DOWN(virt + off, PAGE_SIZE);
        if (!(pml4[INDEX(v, 39)] & PTE_PRESENT))
            continue;
        u64 *pdpt = table_virt(pml4[INDEX(v, 39)]);
        if (!(pdpt[INDEX(v, 30)] & PTE_PRESENT))
            continue;
        u64 *pd = table_virt(pdpt[INDEX(v, 30)]);
        if (!(pd[INDEX(v, 21)] & PTE_PRESENT) || (pd[INDEX(v, 21)] & PTE_PS))
            continue;
        u64 *pt = table_virt(pd[INDEX(v, 21)]);
        if (pt[INDEX(v, 12)] & PTE_PRESENT) {
            pt[INDEX(v, 12)] =
                (pt[INDEX(v, 12)] & PTE_ADDR_MASK) | want;
            invlpg((void *)v);
        }
    }
    spin_unlock(&g_vmm_lock);
    return 0;
}

int vmm_translate(u64 virt, phys_addr_t *out_phys)
{
    u64 *pml4 = (u64 *)phys_to_virt(g_kernel_pml4);
    u64 v = virt;

    if (!(pml4[INDEX(v, 39)] & PTE_PRESENT))
        return -1;
    u64 *pdpt = table_virt(pml4[INDEX(v, 39)]);
    if (!(pdpt[INDEX(v, 30)] & PTE_PRESENT))
        return -1;
    u64 *pd = table_virt(pdpt[INDEX(v, 30)]);
    if (!(pd[INDEX(v, 21)] & PTE_PRESENT))
        return -1;
    if (pd[INDEX(v, 21)] & PTE_PS)
        return -2;                      /* 2 MiB 大页，无 4K 级 PTE */

    u64 *pt = table_virt(pd[INDEX(v, 21)]);
    if (!(pt[INDEX(v, 12)] & PTE_PRESENT))
        return -1;

    if (out_phys)
        *out_phys = (pt[INDEX(v, 12)] & PTE_ADDR_MASK) | (v & (PAGE_SIZE - 1));
    return 0;
}

int vmm_create_address_space(struct address_space *as)
{
    if (!as)
        return -FYX_EINVAL;
    phys_addr_t pml4 = pmm_alloc_frame();
    if (pml4 == PMM_INVALID)
        return -FYX_ENOMEM;
    memset(phys_to_virt(pml4), 0, PAGE_SIZE);

    /* 新地址空间共享内核高半核映射（把内核的 PML4 高半部分拷过去） */
    u64 *src = (u64 *)phys_to_virt(g_kernel_pml4);
    u64 *dst = (u64 *)phys_to_virt(pml4);
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];

    as->pml4 = pml4;
    as->refcount = 1;
    return 0;
}

int vmm_map_in(struct address_space *as, u64 virt, phys_addr_t phys,
               size_t size, u32 flags)
{
    if (!as || as->pml4 == 0 || size == 0)
        return -FYX_EINVAL;
    spin_lock(&g_vmm_lock);
    int r = map_4k_root((u64 *)phys_to_virt(as->pml4),
                        ALIGN_DOWN(virt, PAGE_SIZE),
                        ALIGN_DOWN(phys, PAGE_SIZE),
                        ALIGN_UP(size, PAGE_SIZE), flags);
    g_map_calls++;
    spin_unlock(&g_vmm_lock);
    return r;
}

/* 回收用户空间（PML4 的 0..255 项）里的全部页框，包括数据页与各级页表 */
void vmm_destroy_address_space(struct address_space *as)
{
    if (!as || as->pml4 == 0)
        return;
    /* 只回收页表自身所在帧；用户空间页表在进程退出时另行释放 */
    u64 *pml4 = (u64 *)phys_to_virt(as->pml4);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT))
            continue;
        u64 *pdpt = table_virt(pml4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT))
                continue;
            u64 *pd = table_virt(pdpt[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT))
                    continue;
                if (pd[k] & PTE_PS) {           /* 2 MiB 大页直接回收 */
                    pmm_free_frame(pd[k] & PTE_ADDR_MASK);
                    continue;
                }
                u64 *pt = table_virt(pd[k]);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT))
                        continue;
                    pmm_free_frame(pt[l] & PTE_ADDR_MASK);
                }
                pmm_free_frame(pd[k] & PTE_ADDR_MASK);
            }
            pmm_free_frame(pdpt[j] & PTE_ADDR_MASK);
        }
        pmm_free_frame(pml4[i] & PTE_ADDR_MASK);
    }
    pmm_free_frame(as->pml4);
    as->pml4 = 0;
    as->refcount = 0;
}
