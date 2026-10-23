/*
 * FYXOS — 内存管理：物理帧分配器 / 虚拟内存 / 内核堆
 */
#ifndef FYXOS_MM_H
#define FYXOS_MM_H

#include <fyxos/types.h>
#include <fyxos/boot_info.h>

/* ====================== 虚拟内存标志（架构无关） ======================== */
#define VMM_PRESENT  0x01u
#define VMM_WRITE    0x02u
#define VMM_USER     0x04u
#define VMM_NOCACHE  0x08u
#define VMM_LARGE    0x10u      /* 2 MiB 大页 */
#define VMM_NOEXEC   0x20u

#define PAGE_ENTRIES 512u

/* ============================ 物理内存管理 ============================== */
struct pmm_stats {
    u64 total_bytes;
    u64 free_bytes;
    u64 used_bytes;
    u64 total_frames;
    u64 free_frames;
    u64 reserved_frames;        /* 内核映像 + 引导期数据 + 位图自身 */
    u64 last_allocated;         /* 最后一次分配出的物理地址（调试用） */
};

void pmm_init(struct fyx_boot_info *bi);
void pmm_stats(struct pmm_stats *out);

phys_addr_t pmm_alloc_frame(void);          /* 返回物理地址，失败返回 0 */
phys_addr_t pmm_alloc_frames(size_t count); /* 连续 count 个物理帧 */
void        pmm_free_frame(phys_addr_t addr);
void        pmm_free_frames(phys_addr_t addr, size_t count);
void        pmm_reserve_range(phys_addr_t base, size_t length);
void        pmm_lift_early_limit(void);   /* 由 vmm_init() 在重映射完成后调用 */

#define PMM_INVALID ((phys_addr_t)0)

/* ============================ 虚拟内存管理 ============================== */
void vmm_init(struct fyx_boot_info *bi);
void vmm_switch(phys_addr_t pml4_phys);
phys_addr_t vmm_current_pml4(void);
phys_addr_t vmm_kernel_pml4(void);      /* 内核自身地址空间的 PML4 */

int  vmm_map(u64 virt, phys_addr_t phys, size_t size, u32 flags);
int  vmm_unmap(u64 virt, size_t size);
int  vmm_protect(u64 virt, size_t size, u32 flags);
int  vmm_translate(u64 virt, phys_addr_t *out_phys);

/* 地址空间（本阶段所有线程共享内核地址空间，接口先预留给进程） */
struct address_space {
    phys_addr_t pml4;
    u64         refcount;
};

int  vmm_create_address_space(struct address_space *as);
void vmm_destroy_address_space(struct address_space *as);

/* 往指定地址空间映射（用户进程用；内核空间继续用 vmm_map） */
int  vmm_map_in(struct address_space *as, u64 virt, phys_addr_t phys,
                size_t size, u32 flags);

/* 地址转换辅助 */
static inline void *phys_to_virt(phys_addr_t pa)
{
    return (void *)(uintptr_t)(pa + HHDM_BASE);
}

static inline phys_addr_t virt_to_phys(void *va)
{
    return (phys_addr_t)((uintptr_t)va - HHDM_BASE);
}

/* ================================ 内核堆 ================================ */
void  heap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);

struct heap_stats {
    size_t total;
    size_t used;
    size_t free;
    size_t peak_used;
    u64    alloc_count;
    u64    free_count;
    u64    fail_count;
};

void heap_stats(struct heap_stats *out);

#endif /* FYXOS_MM_H */
