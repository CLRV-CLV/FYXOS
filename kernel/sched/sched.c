/*
 * FYXOS — 抢占式轮转调度器
 * ---------------------------------------------------------------------------
 * 切换时机：定时器中断（PIT）。切换动作发生在中断返回路径上
 * （见 switch.S 的说明），因此每个线程在“不运行”时，栈上都有一份完整的
 * 中断栈帧。
 *
 * 阻塞/退出的实现非常轻：改状态后 sti; hlt —— 下一个时钟中断会把 CPU
 * 交给别的线程，而本线程因为状态不是 RUNNABLE 不会再被选中。
 */
#include <fyxos/sched.h>

#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/mm.h>
#include <fyxos/x86_64.h>
#include <fyxos/syscall.h>
#include <fyxos/sync.h>

#define SCHED_YIELD_VECTOR 0x81

extern u64 isr_stub_table[];
extern void isr_dispatch_return_stub(void);
extern void isr_return_point(void);
extern void thread_trampoline(void);
void irq_context_switch(uintptr_t *prev_slot, uintptr_t next_rsp);

static struct thread   g_threads[MAX_THREADS];
static struct list_head g_runqueue;
static struct list_head g_all;
static struct thread   *g_current;
static struct thread   *g_idle;      /* 只在运行队列为空时才被调度到 */
static u64              g_next_tid = 1;
static bool             g_started;
static bool             g_preempt_enable = true;
static spinlock_t       g_sched_lock = SPINLOCK_INIT("sched");
static struct sched_stats g_stats;

/* ---------------------------------------------------------------- 辅助 */
static struct thread *alloc_thread_slot(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].state == THREAD_UNUSED)
            return &g_threads[i];
    }
    return NULL;
}

static void enqueue_run(struct thread *t)
{
    /*
     * 守卫只能挡"已经在队列里"的情况（THREAD_RUNNABLE）。
     *
     * 这里曾经写成 `RUNNING || RUNNABLE` 直接返回，结果 sched_switch() 里
     * 对被换出线程的 enqueue_run(prev) 永远早退 —— prev 的状态恰好就是
     * RUNNING，于是它再也回不到运行队列，线程被永久丢失：演示里两个线程
     * 各只推进一次就再没被调度过。
     *
     * 正确语义：RUNNING 表示"正在占用 CPU、不在队列中"，正是需要入队的情况。
     * 其余调用点（thread_create / thread_wake / 唤醒睡眠线程）传入的都是
     * EMBRYO / BLOCKED / SLEEPING，不受此守卫影响。
     */
    if (t->state == THREAD_RUNNABLE)
        return;
    t->state = THREAD_RUNNABLE;
    list_insert_before(&g_runqueue, &t->run_link);
    g_stats.nr_runnable++;
}

static struct thread *dequeue_run(void)
{
    if (list_empty(&g_runqueue))
        return NULL;
    struct list_head *n = g_runqueue.next;
    list_remove(n);
    struct thread *t = container_of(n, struct thread, run_link);
    g_stats.nr_runnable--;
    return t;
}

/*
 * 让一条线程重新可运行（唤醒路径专用）。
 *
 * 不变式：一条线程要么作为 g_current 占据"运行位"，要么作为 RUNNABLE 待在
 * 运行队列里 —— 两者只能居其一。
 *
 * 被唤醒的往往正是当前线程：shell 执行 sleep 时睡的就是自己，到期由
 * sched_tick() 唤醒。若此时也把它链进运行队列，同一条线程就有了两处存在：
 * 等它再次睡眠、队列里那一份被 dequeue 出来置成 RUNNING 时，睡眠会莫名其妙
 * 提前结束（真机上更糟 —— 同一个栈会被两个执行流同时使用）。
 *
 * sched_switch() 里换出 prev 的 enqueue_run() 不受影响：那里紧接着就要把
 * g_current 换成 next，prev 入队正是本意。
 */
static void thread_make_runnable(struct thread *t)
{
    if (t == g_current)
        t->state = THREAD_RUNNING;
    else
        enqueue_run(t);
}

static struct thread *pick_next(void)
{
    /*
     * 轮转：从运行队列头取一个。
     *
     * 队列里理论上只有 RUNNABLE 的线程；万一混进了僵尸或睡眠中的项（说明有人
     * 违反了"一条线程只在一处"的不变式），就地丢弃而不是把它当成下一个线程
     * —— 否则会切到一条已经退出/正在睡的线程的栈上去，iretq 直接 #GP。
     *
     * 队列空了就跑 idle，因此这里永远不返回 NULL，"没有可运行线程"不再是
     * 需要调用方兜底的特殊情况。
     */
    for (;;) {
        struct thread *t = dequeue_run();
        if (!t)
            break;
        if (t->state == THREAD_RUNNABLE)
            return t;
    }
    return g_idle;
}

/* ------------------------------------------------------- 新线程的初始栈 */
static void thread_build_initial_stack(struct thread *t, thread_fn entry,
                                       void *arg)
{
    u64 *sp = (u64 *)((uintptr_t)t->stack_base);      /* 栈是高地址向低地址长 */

    /* 从栈顶往下依次布置“被中断时的栈帧”：
     *
     *   高地址  SS
     *          RSP
     *          RFLAGS
     *          CS
     *          RIP   -> thread_trampoline
     *          ERROR_CODE
     *          VECTOR
     *          RAX ... R15
     *   低地址  ...
     *
     * 再往下是 irq_context_switch 的两级返回地址。
     */
    *(--sp) = GDT_SEL_KERNEL_DATA;                    /* ss */
    *(--sp) = (u64)(uintptr_t)t->stack_base;          /* rsp */
    *(--sp) = 0x202ull;                               /* rflags: IF=1 */
    *(--sp) = GDT_SEL_KERNEL_CODE;                    /* cs */
    *(--sp) = (u64)(uintptr_t)thread_trampoline;      /* rip */

    *(--sp) = 0;                                      /* error_code */
    *(--sp) = 0;                                      /* vector */

    /* 通用寄存器：isr_common 的弹出顺序是 r15,r14,...,rax */
    *(--sp) = 0;                                      /* rax */
    *(--sp) = 0;                                      /* rcx */
    *(--sp) = 0;                                      /* rdx */
    *(--sp) = (u64)(uintptr_t)entry;                  /* rbx -> 线程函数 */
    *(--sp) = 0;                                      /* rbp */
    *(--sp) = 0;                                      /* rsi */
    *(--sp) = (u64)(uintptr_t)arg;                    /* rdi -> 参数 */
    for (int i = 8; i <= 15; i++)
        *(--sp) = 0;                                  /* r8..r15 */

    /* 此时 sp 指向 r15，即 isr_common 期望的“帧起点” */

    /* irq_context_switch 的返回链：先回到 isr_dispatch 的调用点，
     * 再回到 isr_common 里 `call isr_dispatch` 之后的那条指令。 */
    *(--sp) = (u64)(uintptr_t)isr_return_point;
    *(--sp) = (u64)(uintptr_t)isr_dispatch_return_stub;

    t->rsp = (u64 *)sp;
}

/* ---------------------------------------------------------------- 接口 */
void sched_init(void)
{
    memset(g_threads, 0, sizeof(g_threads));
    list_init(&g_runqueue);
    list_init(&g_all);
    g_current = NULL;
    g_idle = NULL;
    g_started = false;
    memset(&g_stats, 0, sizeof(g_stats));
}

struct thread *thread_create(const char *name, thread_fn entry, void *arg,
                             int priority)
{
    if (!entry)
        return NULL;

    u64 flags = irq_save_disable();
    spin_lock(&g_sched_lock);

    struct thread *t = alloc_thread_slot();
    if (!t) {
        spin_unlock(&g_sched_lock);
        irq_restore(flags);
        return NULL;
    }

    memset(t, 0, sizeof(*t));
    t->tid = g_next_tid++;
    strncpy(t->name, name ? name : "unnamed", THREAD_NAME_MAX - 1);
    t->priority = priority;
    t->timeslice = DEFAULT_TIMESLICE_MS;
    t->state = THREAD_EMBRYO;

    t->stack_size = THREAD_STACK_SIZE;
    t->stack_base = (u64 *)kmalloc_aligned(t->stack_size, 16);
    if (!t->stack_base) {
        t->state = THREAD_UNUSED;
        spin_unlock(&g_sched_lock);
        irq_restore(flags);
        return NULL;
    }
    memset(t->stack_base, 0, t->stack_size);
    /* stack_base 指向分配区起始，栈顶是它的高地址端 */
    t->stack_base = (u64 *)((uintptr_t)t->stack_base + t->stack_size);

    thread_build_initial_stack(t, entry, arg);

    list_insert_before(&g_all, &t->all_link);
    g_stats.nr_threads++;
    enqueue_run(t);

    spin_unlock(&g_sched_lock);
    irq_restore(flags);
    return t;
}

struct thread *current_thread(void)
{
    return g_current;
}

/*
 * idle 线程：只在运行队列为空时才被 pick_next() 选中。
 *
 * 有了它，"没有可运行线程"就不再是调度器要到处兜底的特殊情况 —— sched_switch()
 * 永远有合法目标，睡眠/阻塞的线程也能真正把 CPU 交出去，而不是留在自己的栈上
 * 空转等时钟。它是唯一一条"恒为 RUNNING 但不在运行队列里"的线程，因此
 * sched_switch() 里换出 prev 时要显式把它排除在入队之外。
 */
static void idle_main(void *arg)
{
    UNUSED(arg);
    for (;;) {
        enable_interrupts();
        cpu_halt();
    }
}

static void sched_create_idle(void)
{
    g_idle = thread_create("idle", idle_main, NULL, 0);
    if (!g_idle)
        panic("sched: no slot for idle thread");

    /* 从运行队列里摘出来：idle 不参与轮转，也不计入可运行数 */
    list_remove(&g_idle->run_link);
    g_stats.nr_runnable--;
    g_idle->state = THREAD_RUNNING;
}

struct thread *thread_by_tid(u64 tid)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].state != THREAD_UNUSED && g_threads[i].tid == tid)
            return &g_threads[i];
    return NULL;
}

void thread_block(struct thread *t)
{
    if (!t)
        return;
    u64 flags = irq_save_disable();
    if (t->state == THREAD_RUNNABLE) {
        list_remove(&t->run_link);
        g_stats.nr_runnable--;
    }
    t->state = THREAD_BLOCKED;
    irq_restore(flags);
}

void thread_wake(struct thread *t)
{
    if (!t)
        return;
    u64 flags = irq_save_disable();
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        t->wake_tick = 0;
        thread_make_runnable(t);
    }
    irq_restore(flags);
}

void thread_yield(void)
{
    if (!g_started)
        return;
    g_stats.yields++;
    if (g_current)
        g_current->timeslice = 0;
    /* 触发一个软件中断，在中断上下文里完成切换 */
    __asm__ volatile("int $0x81" ::: "memory");
}

void thread_sleep_ms(u64 ms)
{
    if (!g_started || ms == 0)
        return;
    u32 freq = pit_frequency() ? pit_frequency() : 100;
    u64 ticks = (ms * freq) / 1000;
    if (ticks == 0)
        ticks = 1;

    /*
     * 必须是 THREAD_SLEEPING，不能走 thread_block() 的 THREAD_BLOCKED。
     *
     * sched_tick() 的到期唤醒只扫描 THREAD_SLEEPING：BLOCKED 语义是"等某个
     * 事件、只有 thread_wake() 能解除"，而睡眠是"等时间、由时钟解除"。这里
     * 曾经误用 thread_block()，于是睡下去就再也没有人唤醒 —— shell 里一条
     * sleep 就能把整个系统挂死，真机同样如此。
     */
    u64 flags = irq_save_disable();
    g_current->wake_tick = pit_ticks() + ticks;
    g_current->state = THREAD_SLEEPING;
    irq_restore(flags);

    /*
     * 真正把 CPU 让出去：切到 idle 线程去 hlt，而不是留在自己的栈上空转。
     * 到期后 sched_tick() 会把它重新入队，下一次调度自然把它切回来，
     * 从下面这条 thread_yield() 之后继续往下走。
     */
    thread_yield();
}

void thread_exit(int code)
{
    if (!g_current) {
        kernel_halt();
    }
    u64 flags = irq_save_disable();
    g_current->exit_code = code;
    g_current->state = THREAD_ZOMBIE;
    g_stats.nr_threads--;
    irq_restore(flags);

    /* 变僵尸后不再被调度；等时钟中断把 CPU 交给别人 */
    for (;;) {
        enable_interrupts();
        cpu_halt();
    }
}

/* ------------------------------------------------- 中断上下文里的切换 */
/*
 * 由 isr_dispatch 在定时器中断（或 yield 软中断）中调用。
 * 返回后，isr_common 会用“新线程”的栈帧恢复寄存器并 iretq。
 */
void sched_switch(void)
{
    if (!g_started || !g_preempt_enable)
        return;

    struct thread *prev = g_current;
    struct thread *next = pick_next();

    /* pick_next() 有 idle 兜底，理论上不会是 NULL；留着以防 idle 还没建好 */
    if (!next)
        return;

    /* 选中的还是自己：刷新时间片即可，不必走切换路径 */
    if (next == prev) {
        next->state = THREAD_RUNNING;
        next->timeslice = (next == g_idle) ? 1 : DEFAULT_TIMESLICE_MS;
        return;
    }

    /* 换出的线程重新入队。idle 不参与轮转，绝不入队。 */
    if (prev) {
        if (prev->state == THREAD_RUNNING && prev != g_idle)
            enqueue_run(prev);
    }

    next->state = THREAD_RUNNING;
    /* idle 只占一个 tick：好让刚被唤醒的线程尽快拿回 CPU */
    next->timeslice = (next == g_idle) ? 1 : DEFAULT_TIMESLICE_MS;
    next->switches++;
    g_current = next;
    g_stats.switches++;

    /*
     * 地址空间与内核栈都要跟着走：
     *   - pml4 为 0 表示沿用内核空间（内核线程），非 0 是用户进程自己的 PML4；
     *   - TSS.RSP0 是 ring 3 陷入时 CPU 取内核栈的地方，必须指向新任务的内核
     *     栈顶，否则用户进程一被中断就压帧压到没有映射的地址上。
     */
    if (next->pml4 != prev->pml4)
        vmm_switch(next->pml4 ? next->pml4 : vmm_kernel_pml4());
    gdt_set_kernel_stack((u64)next->stack_base);
    syscall_set_kernel_stack((u64)next->stack_base);

    irq_context_switch((uintptr_t *)&prev->rsp, (uintptr_t)next->rsp);
}

void sched_tick(void)
{
    g_stats.ticks++;

    /* 唤醒到期的睡眠线程 */
    u64 now = pit_ticks();
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &g_threads[i];
        if (t->state == THREAD_SLEEPING && t->wake_tick &&
            now >= t->wake_tick) {
            t->wake_tick = 0;
            thread_make_runnable(t);
        }
    }

    if (!g_started || !g_current)
        return;

    g_current->total_ticks++;
    if (g_current->timeslice > 0)
        g_current->timeslice--;

    if (g_current->timeslice == 0) {
        g_stats.preemptions++;
        sched_switch();
    }
}

void sched_start(void)
{
    if (g_started)
        return;

    /* 先备好 idle，之后任何"无事可做"的时刻都有落脚点 */
    sched_create_idle();

    /* 把当前执行流（kmain -> shell）登记为一条普通线程 */
    struct thread *boot = alloc_thread_slot();
    if (!boot)
        panic("sched: no slot for boot thread");
    memset(boot, 0, sizeof(*boot));
    boot->tid = 0;
    strncpy(boot->name, "shell/main", THREAD_NAME_MAX - 1);
    boot->state = THREAD_RUNNING;
    boot->priority = 255;
    boot->timeslice = DEFAULT_TIMESLICE_MS;
    boot->stack_base = 0;
    boot->stack_size = 0;
    list_insert_before(&g_all, &boot->all_link);
    g_stats.nr_threads++;

    g_current = boot;
    g_started = true;
    g_stats.idle_ticks = 0;

    enable_interrupts();
}

void sched_stats(struct sched_stats *out)
{
    if (!out)
        return;
    u64 flags = irq_save_disable();
    *out = g_stats;
    out->nr_threads = 0;
    out->nr_runnable = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_threads[i].state == THREAD_UNUSED)
            continue;
        out->nr_threads++;
        if (g_threads[i].state == THREAD_RUNNABLE)
            out->nr_runnable++;
    }
    irq_restore(flags);
}

void switch_to(struct thread *prev, struct thread *next)
{
    /* 保留符号以符合 sched.h 的声明；真正的切换走 irq_context_switch */
    UNUSED(prev);
    UNUSED(next);
}

void NORETURN thread_first_jump(struct thread *t)
{
    UNUSED(t);
    kernel_halt();
}
