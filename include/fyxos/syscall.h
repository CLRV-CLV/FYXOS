/*
 * FYXOS — 系统调用 ABI
 * ---------------------------------------------------------------------------
 * 使用 x86-64 的 SYSCALL / SYSRET 快速系统调用指令。
 *   调用：RAX = 系统调用号，参数依次 RDI, RSI, RDX, R10, R8, R9
 *   返回：RAX = 返回值（负数表示 -errno），RDX 可能作为第二返回值
 *  破坏：RCX（返回地址）、R11（RFLAGS）
 *
 * 这是本阶段的内核态接口；用户态进程（阶段 3）复用同一套编号。
 */
#ifndef FYXOS_SYSCALL_H
#define FYXOS_SYSCALL_H

#include <fyxos/types.h>

#define FYX_SYS_RESERVED_BASE 0x1000   /* 用户可见编号从 0x1000 起 */

enum fyx_syscall {
    SYS_LOG_WRITE   = 0x1000,   /* (const char *buf, size_t len) */
    SYS_CONSOLE_PUT = 0x1001,   /* (int ch) */
    SYS_GET_VERSION = 0x1002,   /* () -> 版本字符串指针 */
    SYS_GET_TICKS   = 0x1003,   /* () -> 系统启动以来的 tick 数 */
    SYS_GET_TID     = 0x1004,   /* () -> 当前线程 ID */
    SYS_THREAD_CREATE = 0x1005, /* (const char *name, void *fn, void *arg) */
    SYS_THREAD_EXIT = 0x1006,   /* (int code) */
    SYS_THREAD_YIELD = 0x1007,  /* () */
    SYS_THREAD_SLEEP = 0x1008,  /* (u64 ms) */
    SYS_HEAP_STATS  = 0x1009,   /* (void *out) */
    SYS_PMM_STATS   = 0x100A,   /* (void *out) */
    SYS_SCHED_STATS = 0x100B,   /* (void *out) */
    SYS_THREAD_LIST = 0x100C,   /* (void *out, u32 max) */
    SYS_SELFTEST    = 0x100D,   /* () -> 运行内核自检，返回失败数 */
    SYS_SHUTDOWN    = 0x100E,   /* (u8 code) */
    SYS_DEBUG_ECHO  = 0x100F,   /* (u64 value) -> 原样返回，用于 ABI 自检 */
    SYS_MAX
};

/* 错误码（返回值取负）定义在 types.h，供全部子系统共用 */

void syscall_init(void);
long syscall_dispatch(u64 number, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

/* 供内核代码直接调用的实现（避免绕一圈 syscall 指令） */
long sys_log_write(const char *buf, size_t len);
u64  syscall_count(void);
u64  syscall_unknown_count(void);

#endif /* FYXOS_SYSCALL_H */
