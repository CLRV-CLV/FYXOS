/*
 * FYXOS — 抢占式调度器
 * ---------------------------------------------------------------------------
 * 本阶段实现内核线程（同一地址空间），采用带优先级的轮转调度（round-robin）
 * 与可配置时间片。调度点为：
 *   - 定时器中断（PIT 100 Hz）触发的时间片耗尽
 *   - 显式 sched_yield()
 *   - 线程阻塞/退出
 */
#ifndef FYXOS_SCHED_H
#define FYXOS_SCHED_H

#include <fyxos/types.h>
#include <fyxos/sync.h>

#define THREAD_NAME_MAX  32
#define THREAD_STACK_SIZE (16 * 1024)
#define MAX_THREADS      64
#define DEFAULT_TIMESLICE_MS 10

typedef enum {
    THREAD_UNUSED = 0,
    THREAD_EMBRYO,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,
    THREAD_ZOMBIE
} thread_state_t;

typedef void (*thread_fn)(void *arg);

struct thread {
    u64             tid;
    char            name[THREAD_NAME_MAX];
    thread_state_t  state;
    int             priority;          /* 0 = 最高，数值越大优先级越低 */
    u64             wake_tick;         /* THREAD_SLEEPING 时的唤醒时刻 */
    int             timeslice;         /* 剩余时间片（tick 数） */
    int             exit_code;

    u64            *stack_base;        /* 内核栈底（高地址） */
    size_t          stack_size;
    u64            *rsp;               /* 被换出时保存的栈指针 */

    u64             total_ticks;       /* 累计占用 CPU 的 tick 数 */
    u64             switches;          /* 被调度次数 */

    struct list_head run_link;
    struct list_head all_link;
} __attribute__((aligned(64)));

/* ================================ 接口 ================================== */
void sched_init(void);
void sched_start(void);                    /* 开启抢占，永不返回 */

struct thread *thread_create(const char *name, thread_fn entry, void *arg,
                             int priority);
void thread_exit(int code) NORETURN;
void thread_yield(void);
void thread_sleep_ms(u64 ms);
void thread_block(struct thread *t);
void thread_wake(struct thread *t);

struct thread *current_thread(void);
struct thread *thread_by_tid(u64 tid);
void sched_tick(void);                     /* 由定时器中断调用 */

/* 调度器统计 */
struct sched_stats {
    u64     ticks;
    u64     switches;
    u64     preemptions;
    u64     yields;
    u32     nr_threads;
    u32     nr_runnable;
    u64     idle_ticks;
};
void sched_stats(struct sched_stats *out);

/* 由 isr_dispatch 在中断上下文调用：选择下一个线程并切换过去 */
void sched_switch(void);

/* 汇编实现（arch/x86_64/switch.S） */
void switch_to(struct thread *prev, struct thread *next);
void NORETURN thread_first_jump(struct thread *t);

#endif /* FYXOS_SCHED_H */
