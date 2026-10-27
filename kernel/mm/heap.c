/*
 * FYXOS — 内核堆
 * ---------------------------------------------------------------------------
 * 首次适配（first-fit）+ 边界合并（coalescing）的显式空闲链表。
 * 空间不足时按 256 KiB 的步长向 vmm 申请更多页。
 *
 * 每个块都带 16 字节的块头，并保证 16 字节对齐。
 * 块头带魔数，kfree 时可以检测重复释放与越界破坏。
 */
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/sync.h>

#define HEAP_VIRT_START  0xFFFFFFFF90000000ull
#define HEAP_INIT_SIZE   (1ull << 20)          /* 1 MiB */
#define HEAP_MAX_SIZE    (64ull << 20)         /* 64 MiB */
#define HEAP_GROW_STEP   (256ull << 10)        /* 256 KiB */

#define BLOCK_MAGIC      0xB10C10C5u
#define BLOCK_ALIGN      16u
#define HEADER_SIZE      ((sizeof(struct heap_block) + BLOCK_ALIGN - 1) \
                          & ~(BLOCK_ALIGN - 1))
#define MIN_PAYLOAD      16u

struct heap_block {
    u32  magic;
    u32  free;
    size_t size;                 /* payload 字节数，不含块头 */
    struct heap_block *next;
    struct heap_block *prev;
};

static struct heap_block *g_free_list;
static struct heap_block *g_first;
static struct heap_block *g_last;
static u64  g_heap_cur;
static u64  g_heap_end;
static bool g_ready;
static spinlock_t g_lock = SPINLOCK_INIT("heap");
static struct heap_stats g_heap_stats;

static inline struct heap_block *payload_to_block(void *p)
{
    return (struct heap_block *)((uintptr_t)p - HEADER_SIZE);
}

static inline void *block_to_payload(struct heap_block *b)
{
    return (void *)((uintptr_t)b + HEADER_SIZE);
}

/* 向 vmm 申请更多虚拟内存，并把新区域做成一个空闲块追加到链表尾 */
static int heap_grow(size_t need)
{
    size_t grow = ALIGN_UP(need + HEADER_SIZE, HEAP_GROW_STEP);
    if (g_heap_cur + grow > HEAP_VIRT_START + HEAP_MAX_SIZE)
        return -1;

    for (size_t off = 0; off < grow; off += PAGE_SIZE) {
        phys_addr_t page = pmm_alloc_frame();
        if (page == PMM_INVALID)
            return -1;
        if (vmm_map(g_heap_cur + off, page, PAGE_SIZE,
                    VMM_WRITE | VMM_NOEXEC) != 0)
            return -1;
        g_heap_stats.total += PAGE_SIZE;
    }

    struct heap_block *b = (struct heap_block *)g_heap_cur;
    b->magic = BLOCK_MAGIC;
    b->free = 1;
    b->size = grow - HEADER_SIZE;
    b->prev = g_last;
    b->next = NULL;
    if (g_last)
        g_last->next = b;
    g_last = b;

    g_heap_cur += grow;
    return 0;
}

void heap_init(void)
{
    memset(&g_heap_stats, 0, sizeof(g_heap_stats));
    g_free_list = NULL;
    g_first = NULL;
    g_last = NULL;
    g_heap_cur = HEAP_VIRT_START;
    g_heap_end = HEAP_VIRT_START + HEAP_MAX_SIZE;

    for (size_t off = 0; off < HEAP_INIT_SIZE; off += PAGE_SIZE) {
        phys_addr_t page = pmm_alloc_frame();
        if (page == PMM_INVALID)
            panic("heap: initial allocation failed");
        if (vmm_map(HEAP_VIRT_START + off, page, PAGE_SIZE,
                    VMM_WRITE | VMM_NOEXEC) != 0)
            panic("heap: initial mapping failed");
    }
    g_heap_stats.total = HEAP_INIT_SIZE;
    g_heap_cur = HEAP_VIRT_START + HEAP_INIT_SIZE;

    struct heap_block *b = (struct heap_block *)HEAP_VIRT_START;
    b->magic = BLOCK_MAGIC;
    b->free = 1;
    b->size = HEAP_INIT_SIZE - HEADER_SIZE;
    b->next = NULL;
    b->prev = NULL;
    g_first = b;
    g_last = b;
    g_free_list = b;
    g_ready = true;
}

static void split_block(struct heap_block *b, size_t size)
{
    size_t rest = b->size - size;
    if (rest < HEADER_SIZE + MIN_PAYLOAD)
        return;                          /* 剩余太小，不分裂 */

    struct heap_block *nb =
        (struct heap_block *)((uintptr_t)block_to_payload(b) + size);
    nb->magic = BLOCK_MAGIC;
    nb->free = 1;
    nb->size = rest - HEADER_SIZE;
    nb->prev = b;
    nb->next = b->next;
    if (nb->next)
        nb->next->prev = nb;
    b->next = nb;
    b->size = size;
    if (g_last == b)
        g_last = nb;
}

void *kmalloc(size_t size)
{
    if (!g_ready || size == 0)
        return NULL;

    size = ALIGN_UP(size, BLOCK_ALIGN);
    if (size < MIN_PAYLOAD)
        size = MIN_PAYLOAD;

    spin_lock(&g_lock);

    struct heap_block *b = g_free_list;
    while (b) {
        if (b->free && b->size >= size)
            goto found;
        b = b->next;
    }

    /* 没有合适的空闲块：扩容后重试一次 */
    if (heap_grow(size) != 0) {
        g_heap_stats.fail_count++;
        spin_unlock(&g_lock);
        return NULL;
    }
    b = g_last;

found:
    b->free = 0;
    split_block(b, size);

    g_heap_stats.used += b->size + HEADER_SIZE;
    g_heap_stats.alloc_count++;
    if (g_heap_stats.used > g_heap_stats.peak_used)
        g_heap_stats.peak_used = g_heap_stats.used;
    g_heap_stats.free = g_heap_stats.total - g_heap_stats.used;

    spin_unlock(&g_lock);
    return block_to_payload(b);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    /* 阶段 1：堆本身已保证 16 字节对齐，更大的对齐需求先做简单实现 */
    if (align <= BLOCK_ALIGN)
        return kmalloc(size);

    void *p = kmalloc(size + align + BLOCK_ALIGN);
    if (!p)
        return NULL;
    uintptr_t v = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    return (void *)v;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    if (n && size > (size_t)-1 / n)
        return NULL;
    return kzalloc(n * size);
}

void kfree(void *ptr)
{
    if (!ptr || !g_ready)
        return;

    struct heap_block *b = payload_to_block(ptr);
    if ((uintptr_t)b < HEAP_VIRT_START || (uintptr_t)b >= g_heap_end)
        return;
    if (b->magic != BLOCK_MAGIC) {
        kprintf("heap: corrupt or double free at %p\n", ptr);
        return;
    }

    spin_lock(&g_lock);
    b->free = 1;
    g_heap_stats.used -= (b->size + HEADER_SIZE);
    g_heap_stats.free = g_heap_stats.total - g_heap_stats.used;
    g_heap_stats.free_count++;

    /* 与后继合并 */
    while (b->next && b->next->free) {
        struct heap_block *n = b->next;
        b->size += n->size + HEADER_SIZE;
        b->next = n->next;
        if (n->next)
            n->next->prev = b;
        if (g_last == n)
            g_last = b;
        n->magic = 0;
    }
    /* 与前驱合并 */
    while (b->prev && b->prev->free) {
        struct heap_block *p = b->prev;
        p->size += b->size + HEADER_SIZE;
        p->next = b->next;
        if (b->next)
            b->next->prev = p;
        if (g_last == b)
            g_last = p;
        b->magic = 0;
        b = p;
    }

    spin_unlock(&g_lock);
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct heap_block *b = payload_to_block(ptr);
    if (b->magic != BLOCK_MAGIC)
        return NULL;
    if (new_size <= b->size)
        return ptr;

    void *np = kmalloc(new_size);
    if (!np)
        return NULL;
    memcpy(np, ptr, b->size);
    kfree(ptr);
    return np;
}

void heap_stats(struct heap_stats *out)
{
    if (!out)
        return;
    spin_lock(&g_lock);
    *out = g_heap_stats;
    spin_unlock(&g_lock);
}
