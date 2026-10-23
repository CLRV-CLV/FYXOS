/*
 * FYXOS — 物理内存帧分配器（位图）
 * ---------------------------------------------------------------------------
 * 以 4 KiB 为一帧，用一位表示占用情况。位图本身紧随内核映像之后放置，
 * 并在初始化时把自己也标记为已占用。
 *
 * 早期限制：vmm_init() 完成之前，只允许分配 4 GiB 以下的帧 —— 因为
 * stage2 建立的引导页表只映射了低端 4 GiB 的 HHDM 窗口，更高的物理地址
 * 暂时无法通过 phys_to_virt() 访问。
 */
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/sync.h>

#define FRAME_SIZE       PAGE_SIZE
#define BITS_PER_WORD    (sizeof(u64) * 8)
#define EARLY_ADDR_LIMIT 0x100000000ull   /* 4 GiB */

extern u8 _kernel_end[];
extern u8 _kernel_phys_end[];

static u64     *g_bitmap;
static u64      g_bitmap_words;
static u64      g_frame_count;
static u64      g_max_addr;
static u64      g_search_hint;
static u64      g_alloc_limit = EARLY_ADDR_LIMIT;
static bool     g_ready;
static spinlock_t g_lock = SPINLOCK_INIT("pmm");

struct pmm_stats g_stats;

static inline void bitmap_set(u64 frame)
{
    g_bitmap[frame / BITS_PER_WORD] |= (1ull << (frame % BITS_PER_WORD));
}

static inline void bitmap_clear(u64 frame)
{
    g_bitmap[frame / BITS_PER_WORD] &= ~(1ull << (frame % BITS_PER_WORD));
}

static inline int bitmap_test(u64 frame)
{
    return (g_bitmap[frame / BITS_PER_WORD] >> (frame % BITS_PER_WORD)) & 1ull;
}

static u64 addr_to_frame(u64 addr)   { return addr / FRAME_SIZE; }
static u64 frame_to_addr(u64 frame)  { return frame * FRAME_SIZE; }

void pmm_init(struct fyx_boot_info *bi)
{
    u64 kernel_phys_end = (u64)(uintptr_t)_kernel_phys_end;

    /* 1. 由 E820 求出最高物理地址 */
    g_max_addr = 0;
    for (u32 i = 0; i < bi->e820_count && i < FYX_E820_MAX; i++) {
        struct fyx_e820_entry *e = &bi->e820[i];
        if (e->length == 0)
            continue;
        u64 end = e->base + e->length;
        if (end > g_max_addr)
            g_max_addr = end;
    }
    if (g_max_addr == 0)
        g_max_addr = 64ull * 1024 * 1024;      /* 兜底 64 MiB */

    g_frame_count = addr_to_frame(g_max_addr);
    g_bitmap_words = (g_frame_count + BITS_PER_WORD - 1) / BITS_PER_WORD;

    /* 2. 位图紧随内核映像之后（4 KiB 对齐） */
    u64 bitmap_phys = ALIGN_UP(kernel_phys_end, FRAME_SIZE);
    g_bitmap = (u64 *)phys_to_virt(bitmap_phys);

    /* 3. 先全部标记为已占用，再放开 E820 的可用区间 */
    memset(g_bitmap, 0xFF, (size_t)(g_bitmap_words * sizeof(u64)));

    u64 total_usable = 0;
    for (u32 i = 0; i < bi->e820_count && i < FYX_E820_MAX; i++) {
        struct fyx_e820_entry *e = &bi->e820[i];
        if (e->type != FYX_MEM_USABLE || e->length == 0)
            continue;
        u64 start = ALIGN_UP(e->base, FRAME_SIZE);
        u64 end   = ALIGN_DOWN(e->base + e->length, FRAME_SIZE);
        if (end <= start)
            continue;
        total_usable += end - start;
        for (u64 f = addr_to_frame(start); f < addr_to_frame(end); f++)
            bitmap_clear(f);
    }

    /* 4. 保留内核映像、位图自身，以及 1 MiB 以下的引导期数据
     *    （stage2 / boot_info / 引导页表 / 内核 ELF 暂存区） */
    u64 reserved_end = ALIGN_UP(bitmap_phys + g_bitmap_words * sizeof(u64),
                                FRAME_SIZE);
    pmm_reserve_range(0, reserved_end);

    if (total_usable > reserved_end)
        g_stats.free_bytes = total_usable - reserved_end;
    else
        g_stats.free_bytes = 0;
    g_stats.total_bytes = total_usable;
    g_stats.used_bytes = reserved_end;
    g_stats.total_frames = g_frame_count;
    g_stats.reserved_frames = addr_to_frame(reserved_end);

    g_search_hint = addr_to_frame(reserved_end);
    g_ready = true;
}

void pmm_reserve_range(phys_addr_t base, size_t length)
{
    if (!g_bitmap)
        return;
    u64 start = ALIGN_DOWN(base, FRAME_SIZE);
    u64 end   = ALIGN_UP((u64)base + length, FRAME_SIZE);
    if (end > g_max_addr)
        end = g_max_addr;
    for (u64 f = addr_to_frame(start); f < addr_to_frame(end); f++)
        bitmap_set(f);
}

static phys_addr_t alloc_frames_locked(size_t count)
{
    if (!g_ready || count == 0)
        return PMM_INVALID;

    u64 limit_frame = addr_to_frame(g_alloc_limit);
    if (limit_frame == 0 || limit_frame > g_frame_count)
        limit_frame = g_frame_count;

    u64 need = count;
    u64 f = (g_search_hint < g_frame_count) ? g_search_hint : 0;

    for (u64 iter = 0; iter < g_frame_count; iter++) {
        if (f + need > limit_frame) {
            f = 0;                       /* 绕回低端继续找 */
            if (f + need > limit_frame)
                return PMM_INVALID;
        }
        if (!bitmap_test(f)) {
            u64 run = 0;
            while (run < need && (f + run) < limit_frame &&
                   !bitmap_test(f + run))
                run++;
            if (run == need) {
                for (u64 i = 0; i < need; i++)
                    bitmap_set(f + i);
                g_search_hint = f + need;
                g_stats.free_bytes -= (u64)count * FRAME_SIZE;
                g_stats.used_bytes += (u64)count * FRAME_SIZE;
                g_stats.last_allocated = frame_to_addr(f);
                return frame_to_addr(f);
            }
            f += run;                    /* 跳过这段碎片 */
            continue;
        }
        f++;
    }
    return PMM_INVALID;
}

phys_addr_t pmm_alloc_frame(void)
{
    spin_lock(&g_lock);
    phys_addr_t r = alloc_frames_locked(1);
    spin_unlock(&g_lock);
    return r;
}

phys_addr_t pmm_alloc_frames(size_t count)
{
    spin_lock(&g_lock);
    phys_addr_t r = alloc_frames_locked(count);
    spin_unlock(&g_lock);
    return r;
}

void pmm_free_frames(phys_addr_t addr, size_t count)
{
    if (!g_ready || addr == 0 || count == 0)
        return;
    spin_lock(&g_lock);
    u64 f = addr_to_frame(addr);
    for (u64 i = 0; i < count; i++)
        bitmap_clear(f + i);
    g_stats.free_bytes += (u64)count * FRAME_SIZE;
    g_stats.used_bytes -= (u64)count * FRAME_SIZE;
    if (f < g_search_hint)
        g_search_hint = f;
    spin_unlock(&g_lock);
}

void pmm_free_frame(phys_addr_t addr)
{
    pmm_free_frames(addr, 1);
}

void pmm_stats(struct pmm_stats *out)
{
    if (!out)
        return;
    spin_lock(&g_lock);
    u64 free_frames = 0;
    for (u64 i = 0; i < g_frame_count; i++)
        if (!bitmap_test(i))
            free_frames++;
    g_stats.free_frames = free_frames;
    *out = g_stats;
    spin_unlock(&g_lock);
}

/* vmm_init() 完成后调用：解除 4 GiB 的早期分配限制 */
void pmm_lift_early_limit(void)
{
    g_alloc_limit = g_max_addr;
}
