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

extern void syscall_entry(void);

/* syscall_entry.S 使用的每 CPU 暂存区与内核栈 */
u64 g_syscall_scratch[8] __attribute__((aligned(16)));
u64 g_syscall_stack[1024] __attribute__((aligned(16)));

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

    default:
        g_syscall_unknown++;
        return -FYX_ENOSYS;
    }
}

void syscall_init(void)
{
    g_syscall_count = 0;
    g_syscall_unknown = 0;

    if (!g_cpu.has_syscall) {
        kprintf("  syscall: CPU 不支持 SYSCALL/SYSRET，仅保留内核直接调用路径\n");
        return;
    }
    gdt_setup_syscall_msrs((void *)syscall_entry);
}

u64 syscall_count(void)      { return g_syscall_count; }
u64 syscall_unknown_count(void) { return g_syscall_unknown; }
