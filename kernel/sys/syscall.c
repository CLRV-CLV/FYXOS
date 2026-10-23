/*
 * FYXOS — 系统调用实现
 * ---------------------------------------------------------------------------
 * 阶段 1 还没有用户态进程，但仍完整实现了 SYSCALL/SYSRET 的 ABI 与分发表：
 * 内核线程可以直接调用 syscall_dispatch() 走同一条路径，保证这条 ABI
 * 在进入用户态之前就已经被真实地执行与测试过。
 */
#include <fyxos/syscall.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/sched.h>
#include <fyxos/x86_64.h>
#include <fyxos/process.h>
#include <fyxos/pipe.h>

extern void syscall_entry(void);

/* syscall_entry.S 使用的暂存区，以及当前线程的内核栈顶 */
u64 g_syscall_scratch[8] __attribute__((aligned(16)));
u64 g_syscall_stack[1024] __attribute__((aligned(16)));
u64 g_syscall_stack_top __attribute__((aligned(16)));

/*
 * 切换当前系统调用栈。栈顶为 0 时表示引导线程，使用静态兜底栈；
 * 用户线程和 idle 线程都有自己的 16 KiB 内核栈。
 */
void syscall_set_kernel_stack(u64 stack_top)
{
    g_syscall_stack_top = stack_top ? stack_top :
        (u64)(uintptr_t)&g_syscall_stack[1024];
}

/*
 * 用户态现场。由 syscall_entry 在汇编里完整填充，然后拷给当前线程。
 * fork 靠它给子进程造一份"从 fork 返回 0"的假象。
 */
struct user_regs g_user_regs __attribute__((aligned(16)));

static u64 g_syscall_count;
static u64 g_syscall_unknown;

long sys_log_write(const char *buf, size_t len)
{
    if (!buf)
        return -FYX_EFAULT;
    console_write(buf, len);
    return (long)len;
}

static long sys_get_version(void)
{
    return (long)(uintptr_t)FYXOS_VERSION;
}

static long sys_thread_create(const char *name, void *fn, void *arg)
{
    if (!fn)
        return -FYX_EINVAL;
    struct thread *t = thread_create(name ? name : "syscall",
                                     (thread_fn)fn, arg, 0);
    if (!t)
        return -FYX_ENOMEM;
    return (long)t->tid;
}

long syscall_dispatch(u64 number, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                      u64 a6)
{
    UNUSED(a4);
    UNUSED(a5);
    UNUSED(a6);
    g_syscall_count++;

    /*
     * 把刚陷入时的用户现场存到当前线程上。
     *
     * 只有真正从 ring 3 进来才有意义；内核线程直接调用本函数时 g_user_regs
     * 里是上一次的内容，但内核线程的 uregs 不会被用到（它们不返回用户态），
     * 所以无条件拷贝是安全的、也省得再判断来源。
     */
    {
        struct thread *t = current_thread();
        if (t)
            t->uregs = g_user_regs;
    }

    switch (number) {
    case SYS_LOG_WRITE:
        return sys_log_write((const char *)(uintptr_t)a1, (size_t)a2);

    case SYS_CONSOLE_PUT:
        console_putc((char)a1);
        return FYX_OK;

    case SYS_GET_VERSION:
        return sys_get_version();

    case SYS_GET_TICKS:
        return (long)pit_ticks();

    case SYS_GET_TID: {
        struct thread *t = current_thread();
        return t ? (long)t->tid : 0;
    }

    case SYS_THREAD_CREATE:
        return sys_thread_create((const char *)(uintptr_t)a1,
                                 (void *)(uintptr_t)a2,
                                 (void *)(uintptr_t)a3);

    case SYS_THREAD_EXIT:
        thread_exit((int)a1);
        return FYX_OK;                 /* 不会到达 */

    case SYS_THREAD_YIELD:
        thread_yield();
        return FYX_OK;

    case SYS_THREAD_SLEEP:
        thread_sleep_ms(a1);
        return FYX_OK;

    case SYS_HEAP_STATS: {
        struct heap_stats st;
        heap_stats(&st);
        if (!a1)
            return -FYX_EFAULT;
        memcpy((void *)(uintptr_t)a1, &st, sizeof(st));
        return FYX_OK;
    }

    case SYS_PMM_STATS: {
        struct pmm_stats st;
        pmm_stats(&st);
        if (!a1)
            return -FYX_EFAULT;
        memcpy((void *)(uintptr_t)a1, &st, sizeof(st));
        return FYX_OK;
    }

    case SYS_SCHED_STATS: {
        struct sched_stats st;
        sched_stats(&st);
        if (!a1)
            return -FYX_EFAULT;
        memcpy((void *)(uintptr_t)a1, &st, sizeof(st));
        return FYX_OK;
    }

    case SYS_THREAD_LIST: {
        if (!a1)
            return -FYX_EINVAL;
        struct thread_info {
            u64  tid;
            char name[THREAD_NAME_MAX];
            u32  state;
            u64  ticks;
            u64  switches;
        } PACKED;
        struct thread_info *out = (struct thread_info *)(uintptr_t)a1;
        u32 max = (u32)a2;
        u32 n = 0;
        for (u64 tid = 0; tid < MAX_THREADS + 1 && n < max; tid++) {
            struct thread *t = thread_by_tid(tid);
            if (!t)
                continue;
            out[n].tid = t->tid;
            memcpy(out[n].name, t->name, THREAD_NAME_MAX);
            out[n].state = (u32)t->state;
            out[n].ticks = t->total_ticks;
            out[n].switches = t->switches;
            n++;
        }
        return (long)n;
    }

    case SYS_SELFTEST:
        return (long)kernel_selftest();

    case SYS_SHUTDOWN:
        arch_shutdown((u8)a1);
        return FYX_OK;

    case SYS_DEBUG_ECHO:
        return (long)a1;                /* ABI 自检：原样返回 */

    /* ---- 用户进程 ---- */
    case SYS_FORK:
        return sys_fork();

    case SYS_EXEC:
        return sys_exec((const char *)(uintptr_t)a1,
                        (const char *const *)(uintptr_t)a2,
                        (const char *const *)(uintptr_t)a3);

    case SYS_WAIT:
        return sys_wait((u64)a1, (int *)(uintptr_t)a2);

    case SYS_EXIT:
        sys_exit((int)a1);
        return FYX_OK;                  /* 不会到达 */

    case SYS_GETPID:
        return sys_getpid();

    case SYS_GETPPID:
        return sys_getppid();

    case SYS_BRK:
        return sys_brk((u64)a1);

    case SYS_PIPE:
        return sys_pipe((int *)(uintptr_t)a1);

    case SYS_READ:
        return sys_fd_read((int)a1, (void *)(uintptr_t)a2, (size_t)a3);

    case SYS_WRITE:
        return sys_fd_write((int)a1, (const void *)(uintptr_t)a2, (size_t)a3);

    case SYS_CLOSE:
        return sys_close((int)a1);

    case SYS_OPEN:
        return sys_open((const char *)(uintptr_t)a1, (u32)a2, (u32)a3);

    case SYS_LSEEK:
        return sys_lseek((int)a1, (i64)a2, (u32)a3);

    case SYS_STAT:
        return sys_stat((const char *)(uintptr_t)a1,
                        (void *)(uintptr_t)a2);

    case SYS_MKDIR:
        return sys_mkdir((const char *)(uintptr_t)a1, (u32)a2);

    case SYS_UNLINK:
        return sys_unlink((const char *)(uintptr_t)a1);

    case SYS_SYNC:
        return sys_sync();

    default:
        g_syscall_unknown++;
        return -FYX_ENOSYS;
    }
}

void syscall_init(void)
{
    g_syscall_count = 0;
    g_syscall_unknown = 0;
    syscall_set_kernel_stack(0);

    if (!g_cpu.has_syscall) {
        kprintf("  syscall: CPU 不支持 SYSCALL/SYSRET，仅保留内核直接调用路径\n");
        return;
    }
    gdt_setup_syscall_msrs((void *)syscall_entry);
}

u64 syscall_count(void)      { return g_syscall_count; }
u64 syscall_unknown_count(void) { return g_syscall_unknown; }
