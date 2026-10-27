/*
 * FYXOS — Linux x86_64 静态用户程序兼容纵切片
 *
 * 这不是完整 Linux ABI：只实现已明确列出的单线程、静态 ELF 所需调用。
 * 未识别调用返回 Linux -ENOSYS，并按调用号打印一次诊断，绝不静默成功。
 */
#include <fyxos/syscall.h>
#include <fyxos/process.h>
#include <fyxos/pipe.h>
#include <fyxos/vfs.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/x86_64.h>

#define LINUX_EPERM        1
#define LINUX_ENOENT       2
#define LINUX_EIO          5
#define LINUX_EBADF        9
#define LINUX_EAGAIN      11
#define LINUX_ENOMEM      12
#define LINUX_EFAULT      14
#define LINUX_EBUSY       16
#define LINUX_EEXIST      17
#define LINUX_ENOTDIR     20
#define LINUX_EISDIR      21
#define LINUX_EINVAL      22
#define LINUX_ENOTTY      25
#define LINUX_ENOSPC      28
#define LINUX_EPIPE       32
#define LINUX_ERANGE      34
#define LINUX_ENOSYS      38
#define LINUX_ENOTEMPTY   39
#define LINUX_EPROTO      71
#define LINUX_EADDRINUSE  98
#define LINUX_ENETUNREACH 101
#define LINUX_ENOTCONN   107
#define LINUX_ETIMEDOUT  110

#define LINUX_AT_FDCWD   (-100)

struct linux_timespec {
    i64 sec;
    i64 nsec;
};

/* Linux x86_64 glibc/kernel 共用的 144 字节 stat ABI。 */
struct linux_stat {
    u64 dev;
    u64 ino;
    u64 nlink;
    u32 mode;
    u32 uid;
    u32 gid;
    u32 pad0;
    u64 rdev;
    i64 size;
    i64 blksize;
    i64 blocks;
    struct linux_timespec atime;
    struct linux_timespec mtime;
    struct linux_timespec ctime;
    i64 reserved[3];
};

struct linux_rlimit {
    u64 cur;
    u64 max;
};

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static u64 g_linux_unknown;
static u64 g_linux_unknown_seen[8]; /* nr 0..511，每个只打印一次 */

static long linux_result(long rc)
{
    if (rc >= 0)
        return rc;
    switch ((int)-rc) {
    case FYX_EINVAL:      return -LINUX_EINVAL;
    case FYX_ENOMEM:      return -LINUX_ENOMEM;
    case FYX_ENOSYS:      return -LINUX_ENOSYS;
    case FYX_EAGAIN:      return -LINUX_EAGAIN;
    case FYX_EFAULT:      return -LINUX_EFAULT;
    case FYX_EPIPE:       return -LINUX_EPIPE;
    case FYX_EIO:         return -LINUX_EIO;
    case FYX_ENOENT:      return -LINUX_ENOENT;
    case FYX_EEXIST:      return -LINUX_EEXIST;
    case FYX_ENOTDIR:     return -LINUX_ENOTDIR;
    case FYX_EISDIR:      return -LINUX_EISDIR;
    case FYX_ENOSPC:      return -LINUX_ENOSPC;
    case FYX_EBADF:       return -LINUX_EBADF;
    case FYX_ENOTEMPTY:   return -LINUX_ENOTEMPTY;
    case FYX_ERANGE:      return -LINUX_ERANGE;
    case FYX_EBUSY:       return -LINUX_EBUSY;
    case FYX_ENETUNREACH: return -LINUX_ENETUNREACH;
    case FYX_ETIMEDOUT:   return -LINUX_ETIMEDOUT;
    case FYX_EADDRINUSE:  return -LINUX_EADDRINUSE;
    case FYX_ENOTCONN:    return -LINUX_ENOTCONN;
    case FYX_EPROTO:      return -LINUX_EPROTO;
    default:              return -LINUX_EPERM;
    }
}

static long linux_unknown(u64 nr)
{
    struct process *p = current_process();
    g_linux_unknown++;
    if (nr < 512) {
        u64 bit = 1ull << (nr & 63);
        u64 *word = &g_linux_unknown_seen[nr >> 6];
        if (!(*word & bit)) {
            *word |= bit;
            kprintf("[linux] ENOSYS pid=%llu nr=%llu（未实现的 Linux 调用）\n",
                    (unsigned long long)(p ? p->pid : 0),
                    (unsigned long long)nr);
        }
    } else {
        kprintf("[linux] ENOSYS pid=%llu nr=%llu（超范围）\n",
                (unsigned long long)(p ? p->pid : 0),
                (unsigned long long)nr);
    }
    return -LINUX_ENOSYS;
}

static long linux_write(int fd, const void *buf, size_t len)
{
    struct process *p = current_process();
    if (!buf && len)
        return -LINUX_EFAULT;
    if (p && (fd == 1 || fd == 2) && p->fds[fd].type == FYX_FD_NONE) {
        console_write((const char *)buf, len);
        return (long)len;
    }
    return linux_result(sys_fd_write(fd, buf, len));
}

static long linux_read(int fd, void *buf, size_t len)
{
    struct process *p = current_process();
    if (!buf && len)
        return -LINUX_EFAULT;
    if (p && fd == 0 && p->fds[0].type == FYX_FD_NONE)
        return 0; /* 当前无交互 stdin 流；明确表现为 EOF */
    return linux_result(sys_fd_read(fd, buf, len));
}

static void linux_stat_console(struct linux_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->ino = 1;
    st->nlink = 1;
    st->mode = 0020000u | 0600u; /* S_IFCHR */
    st->rdev = 0x0100;
    st->blksize = 4096;
}

static long linux_stat_fd(int fd, struct linux_stat *st)
{
    struct process *p = current_process();
    if (!p || !st || fd < 0 || fd >= PROC_MAX_FDS)
        return -LINUX_EBADF;
    if ((fd == 1 || fd == 2) && p->fds[fd].type == FYX_FD_NONE) {
        linux_stat_console(st);
        return 0;
    }
    if (p->fds[fd].type != FYX_FD_FILE || !p->fds[fd].file)
        return -LINUX_EBADF;

    const struct fyx_file *file = p->fds[fd].file;
    memset(st, 0, sizeof(*st));
    st->ino = file->node;
    st->nlink = 1;
    st->mode = (file->type == FYXFS_NODE_DIR ? 0040000u : 0100000u) | 0600u;
    st->blksize = 4096;
    /* fyx_file 不缓存尺寸；此切片不把偏移错误地冒充 st_size。 */
    return 0;
}

static long linux_stat_path(const char *path, struct linux_stat *st)
{
    if (!path || !st)
        return -LINUX_EFAULT;
    struct vfs_stat native;
    long rc = linux_result(vfs_stat(path, &native));
    if (rc < 0)
        return rc;
    memset(st, 0, sizeof(*st));
    st->ino = native.node;
    st->nlink = 1;
    st->mode = (native.type == FYXFS_NODE_DIR ? 0040000u : 0100000u) | 0600u;
    st->size = (i64)native.size;
    st->blksize = 4096;
    st->blocks = (i64)((native.size + 511) / 512);
    return 0;
}

static long linux_readlink(const char *path, char *buf, size_t size)
{
    struct process *p = current_process();
    if (!path || !buf)
        return -LINUX_EFAULT;
    if (strcmp(path, "/proc/self/exe") != 0)
        return -LINUX_ENOENT;
    if (!p || size == 0)
        return -LINUX_EINVAL;
    size_t len = strlen(p->name);
    size_t n = MIN(len, size);
    memcpy(buf, p->name, n); /* readlink 不写 NUL */
    return (long)n;
}

static long linux_openat(int dirfd, const char *path, u32 flags, u32 mode)
{
    if (!path)
        return -LINUX_EFAULT;
    if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
        return -LINUX_ENOTDIR;
    u32 native = flags & 3u;
    if (flags & 0100u)  native |= VFS_O_CREAT;
    if (flags & 01000u) native |= VFS_O_TRUNC;
    if (flags & 02000u) native |= VFS_O_APPEND;
    /* O_DIRECTORY/O_NOFOLLOW 等尚未实现；不接受会改变安全语义的位。 */
    if (flags & ~(3u | 0100u | 01000u | 02000u))
        return -LINUX_EINVAL;
    return linux_result(sys_open(path, native, mode));
}

static long linux_arch_prctl(u64 code, u64 addr)
{
    struct thread *t = current_thread();
    if (!t)
        return -LINUX_EFAULT;
    if (code == 0x1002) { /* ARCH_SET_FS */
        if (addr == 0 || addr >= USER_AREA_LIMIT)
            return -LINUX_EINVAL;
        t->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    }
    if (code == 0x1003) { /* ARCH_GET_FS */
        if (!addr)
            return -LINUX_EFAULT;
        *(u64 *)(uintptr_t)addr = t->fs_base;
        return 0;
    }
    return -LINUX_EINVAL;
}

static long linux_prlimit(u64 pid, u32 resource,
                          const struct linux_rlimit *new_limit,
                          struct linux_rlimit *old_limit)
{
    if (pid != 0 || resource != 3 || new_limit) /* RLIMIT_STACK，只读查询 */
        return -LINUX_EINVAL;
    if (old_limit) {
        old_limit->cur = 8u * 1024u * 1024u;
        old_limit->max = ~0ull;
    }
    return 0;
}

static long linux_clock_gettime(u64 clock_id, struct linux_timespec *out)
{
    if (!out)
        return -LINUX_EFAULT;
    /* 只有由 PIT 真实维护的单调时间；没有 RTC 时不伪造 realtime。 */
    if (clock_id != 1 && clock_id != 4 && clock_id != 7)
        return linux_unknown(228);
    u32 frequency = pit_frequency();
    if (!frequency)
        return -LINUX_EIO;
    u64 ticks = pit_ticks();
    out->sec = (i64)(ticks / frequency);
    out->nsec = (i64)(((ticks % frequency) * 1000000000ull) / frequency);
    return 0;
}

static long linux_uname(struct linux_utsname *out)
{
    if (!out)
        return -LINUX_EFAULT;
    memset(out, 0, sizeof(*out));
    strncpy(out->sysname, "FYXOS", sizeof(out->sysname) - 1);
    strncpy(out->nodename, "fyxos", sizeof(out->nodename) - 1);
    strncpy(out->release, "0.1.0", sizeof(out->release) - 1);
    strncpy(out->version, "Linux-x86_64-compat-static", sizeof(out->version) - 1);
    strncpy(out->machine, "x86_64", sizeof(out->machine) - 1);
    return 0;
}

long linux_syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3,
                            u64 a4, u64 a5, u64 a6)
{
    switch (nr) {
    case 0:   return linux_read((int)a1, (void *)(uintptr_t)a2, (size_t)a3);
    case 1:   return linux_write((int)a1, (const void *)(uintptr_t)a2, (size_t)a3);
    case 3:   return linux_result(sys_close((int)a1));
    case 4:   return linux_stat_path((const char *)(uintptr_t)a1,
                                     (struct linux_stat *)(uintptr_t)a2);
    case 6:   return linux_stat_path((const char *)(uintptr_t)a1,
                                     (struct linux_stat *)(uintptr_t)a2);
    case 8:   return linux_result(sys_lseek((int)a1, (i64)a2, (u32)a3));
    case 9:   return linux_result(sys_mmap(a1, (size_t)a2, (u32)a3, (u32)a4,
                                           (int)a5, a6));
    case 10:  return linux_result(sys_mprotect(a1, (size_t)a2, (u32)a3));
    case 11:  return linux_result(sys_munmap(a1, (size_t)a2));
    case 12:  return sys_brk(a1); /* Linux brk 失败返回旧 break，不走 -errno */
    case 14:                        /* rt_sigprocmask：单线程启动查询/屏蔽 */
      {
        if (a4 != 8 || a1 > 2)
            return -LINUX_EINVAL;
        if (a3)
            *(u64 *)(uintptr_t)a3 = 0;
        return 0;                   /* 当前 ABI 没有异步信号交付 */
      }
    case 16:  return -LINUX_ENOTTY;  /* ioctl：控制台尚无 termios */
    case 39:  return sys_getpid();
    case 60:
    case 231: sys_exit((int)a1); return 0;
    case 63:  return linux_uname((struct linux_utsname *)(uintptr_t)a1);
    case 79:                        /* getcwd */
      {
        char *out = (char *)(uintptr_t)a1;
        if (!out || a2 < 2)
            return -LINUX_ERANGE;
        out[0] = '/'; out[1] = 0;
        return 2;
      }
    case 89:  return linux_readlink((const char *)(uintptr_t)a1,
                                    (char *)(uintptr_t)a2, (size_t)a3);
    case 102:
    case 104:
    case 107:
    case 108:
    case 110:
    case 111: return 0;              /* uid/gid/euid/egid/groups 的早期根身份 */
    case 158:                       /* arch_prctl */
        return linux_arch_prctl(a1, a2);
    case 186:                       /* gettid */
      {
        struct thread *t = current_thread();
        return t ? (long)t->tid : -LINUX_EFAULT;
      }
    case 218:                       /* set_tid_address：一进程一线程 */
        return sys_getpid();
    case 228:
        return linux_clock_gettime(a1, (struct linux_timespec *)(uintptr_t)a2);
    case 257:
        return linux_openat((int)a1, (const char *)(uintptr_t)a2,
                            (u32)a3, (u32)a4);
    case 262:                       /* newfstatat */
        if (((int)a1 != LINUX_AT_FDCWD &&
             ((const char *)(uintptr_t)a2)[0] != '/') || (a4 & ~0x100u))
            return -LINUX_EINVAL;
        return linux_stat_path((const char *)(uintptr_t)a2,
                               (struct linux_stat *)(uintptr_t)a3);
    case 267:                       /* readlinkat */
        if ((int)a1 != LINUX_AT_FDCWD &&
            ((const char *)(uintptr_t)a2)[0] != '/')
            return -LINUX_ENOTDIR;
        return linux_readlink((const char *)(uintptr_t)a2,
                              (char *)(uintptr_t)a3, (size_t)a4);
    case 273:                       /* set_robust_list */
        return a2 == 24 ? 0 : -LINUX_EINVAL;
    case 302:
        return linux_prlimit(a1, (u32)a2,
                             (const struct linux_rlimit *)(uintptr_t)a3,
                             (struct linux_rlimit *)(uintptr_t)a4);
    case 5:                         /* fstat */
        return linux_stat_fd((int)a1, (struct linux_stat *)(uintptr_t)a2);
    default:
        return linux_unknown(nr);
    }
}

u64 linux_syscall_unknown_count(void)
{
    return g_linux_unknown;
}
