/*
 * FYXOS — 同步原语与侵入式链表
 */
#ifndef FYXOS_SYNC_H
#define FYXOS_SYNC_H

#include <fyxos/types.h>

/* ============================== 自旋锁 ================================== */
typedef struct spinlock {
    volatile u32 locked;
    const char  *name;
    u64          owner;        /* 持有者的线程 ID，调试用 */
    u64          acquire_count;
} spinlock_t;

#define SPINLOCK_INIT(nm) { 0, (nm), 0, 0 }

static inline void spinlock_init(spinlock_t *lock, const char *name)
{
    lock->locked = 0;
    lock->name = name;
    lock->owner = 0;
    lock->acquire_count = 0;
}

static inline int spin_trylock(spinlock_t *lock)
{
    u32 expected = 0;
    return __atomic_compare_exchange_n(&lock->locked, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void spin_lock(spinlock_t *lock)
{
    while (!spin_trylock(lock)) {
        /* 提示 CPU 处于自旋，降低功耗与总线争用 */
        __asm__ volatile("pause" ::: "memory");
    }
    lock->acquire_count++;
}

static inline void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

/* 关中断版本：用于中断上下文与进程上下文共享的数据 */
static inline u64 irq_save_disable(void)
{
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(u64 flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

/* ============================ 侵入式双向链表 ============================
 * 链表节点嵌在宿主结构里，不额外分配内存。
 * ---------------------------------------------------------------------- */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

static inline void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

static inline void list_insert_after(struct list_head *node,
                                     struct list_head *new_node)
{
    new_node->next = node->next;
    new_node->prev = node;
    node->next->prev = new_node;
    node->next = new_node;
}

static inline void list_insert_before(struct list_head *node,
                                      struct list_head *new_node)
{
    list_insert_after(node->prev, new_node);
}

static inline void list_remove(struct list_head *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, n, head)                                     \
    for ((pos) = (head)->next, (n) = (pos)->next;                            \
         (pos) != (head); (pos) = (n), (n) = (pos)->next)

#endif /* FYXOS_SYNC_H */
