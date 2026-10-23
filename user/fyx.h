/*
 * FYXOS — 用户态最小运行库（阶段 G 的 libc 雏形）
 * ---------------------------------------------------------------------------
 * 只做一件事：把 SYSCALL 指令包成 C 函数。等 fyx-libc 成型后这份代码会被
 * 真正的头/库取代，但调用约定不变 —— 编号与内核 include/fyxos/syscall.h
 * 严格对应。
 */
#ifndef FYX_USER_H
#define FYX_USER_H

typedef unsigned long size_t_;
typedef long          ssize_t_;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* 系统调用号：必须与 include/fyxos/syscall.h 一一对应 */
#define SYS_LOG_WRITE   0x1000
#define SYS_CONSOLE_PUT 0x1001
#define SYS_GET_TICKS   0x1003
#define SYS_GET_TID     0x1004
#define SYS_SHUTDOWN    0x100E
#define SYS_DEBUG_ECHO  0x100F
#define SYS_FORK        0x1010
#define SYS_EXEC        0x1011
#define SYS_WAIT        0x1012
#define SYS_EXIT        0x1013
#define SYS_GETPID      0x1014
#define SYS_GETPPID     0x1015
#define SYS_BRK         0x1016
#define SYS_PIPE        0x1017
#define SYS_READ        0x1018
#define SYS_WRITE       0x1019
#define SYS_CLOSE       0x101A
#define SYS_OPEN        0x101B
#define SYS_LSEEK       0x101C
#define SYS_STAT        0x101D
#define SYS_MKDIR       0x101E
#define SYS_UNLINK      0x101F
#define SYS_SYNC        0x1020

#define FYX_O_RDONLY    0x00
#define FYX_O_WRONLY    0x01
#define FYX_O_RDWR      0x02
#define FYX_O_CREAT     0x10
#define FYX_O_TRUNC     0x20
#define FYX_O_APPEND    0x40
#define FYX_SEEK_SET    0
#define FYX_SEEK_CUR    1
#define FYX_SEEK_END    2

struct fyx_user_stat {
    unsigned long size;
    unsigned int node;
    unsigned int type;
    unsigned int flags;
    unsigned int crc32;
};
/*
 * SYSCALL 的寄存器约定（与内核 syscall_entry.S 一致）：
 *   号 RAX；参数 RDI, RSI, RDX, R10, R8, R9；
 *   RCX 与 R11 被硬件改写（分别变成返回地址与 RFLAGS），因此列进破坏列表。
 */
static inline long fyx_syscall(long n, long a1, long a2, long a3,
                               long a4, long a5, long a6)
{
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

#define SYS0(n)            fyx_syscall((n), 0, 0, 0, 0, 0, 0)
#define SYS1(n, a)         fyx_syscall((n), (long)(a), 0, 0, 0, 0, 0)
#define SYS2(n, a, b)      fyx_syscall((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define SYS3(n, a, b, c)   fyx_syscall((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

static inline long sys_write(const char *s, unsigned long n)
{
    return SYS2(SYS_LOG_WRITE, s, n);
}
static inline long sys_puts(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return SYS2(SYS_LOG_WRITE, s, n);
}
static inline long sys_exit(int code)      { return SYS1(SYS_EXIT, code); }
static inline long sys_fork(void)          { return SYS0(SYS_FORK); }
static inline long sys_getpid(void)        { return SYS0(SYS_GETPID); }
static inline long sys_getppid(void)       { return SYS0(SYS_GETPPID); }
static inline long sys_get_ticks(void)     { return SYS0(SYS_GET_TICKS); }
static inline long sys_get_tid(void)       { return SYS0(SYS_GET_TID); }
static inline long sys_wait(long pid, int *status)
{
    return SYS2(SYS_WAIT, pid, status);
}
static inline long sys_exec(const char *name, const char *const argv[],
                            const char *const envp[])
{
    return SYS3(SYS_EXEC, name, argv, envp);
}
static inline long sys_brk(unsigned long b) { return SYS1(SYS_BRK, b); }
static inline long sys_pipe(int fds[2])     { return SYS1(SYS_PIPE, fds); }
static inline long sys_fd_read(int fd, void *buf, unsigned long len)
{
    return SYS3(SYS_READ, fd, buf, len);
}
static inline long sys_fd_write(int fd, const void *buf, unsigned long len)
{
    return SYS3(SYS_WRITE, fd, buf, len);
}
static inline long sys_close(int fd)        { return SYS1(SYS_CLOSE, fd); }
static inline long sys_open(const char *path, unsigned long flags)
{
    return SYS3(SYS_OPEN, path, flags, 0);
}
static inline long sys_lseek(int fd, long offset, unsigned long whence)
{
    return SYS3(SYS_LSEEK, fd, offset, whence);
}
static inline long sys_stat(const char *path, struct fyx_user_stat *st)
{
    return SYS2(SYS_STAT, path, st);
}
static inline long sys_mkdir(const char *path)
{
    return SYS2(SYS_MKDIR, path, 0);
}
static inline long sys_unlink(const char *path) { return SYS1(SYS_UNLINK, path); }
static inline long sys_sync(void)               { return SYS0(SYS_SYNC); }
static inline long sys_shutdown(long code)  { return SYS1(SYS_SHUTDOWN, code); }

/* --- 几个不依赖内核的小工具，免得引入外部 libc --- */
static inline unsigned long fyx_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void fyx_put_u64(unsigned long v)
{
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    sys_write(buf + i, (unsigned long)(fyx_strlen(buf + i)));
}

#endif /* FYX_USER_H */
