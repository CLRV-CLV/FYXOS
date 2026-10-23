/*
 * FYXOS — 环形缓冲区管道
 * ---------------------------------------------------------------------------
 * 阶段 G 只需要可靠地完成父子进程通信，因此选择固定容量的内核环形缓冲区，
 * 读者/写者在资源暂时不可用时任由调度器睡眠一个 tick 后重试。
 *
 * 生命周期规则：
 *   - pipe() 在一个进程里安装读、写两个 fd；
 *   - fork 复制 fd，并分别增加 readers/writers；
 *   - exec 不碰 fd 表，管道因此可以跨 exec 使用；
 *   - 最后一个写端关闭后，读端把剩余数据读完再返回 0（EOF）；
 *   - 最后一个端点关闭后释放管道对象。
 *
 * TODO: 阶段 H 用统一的 file/vnode 与条件变量替换轮询睡眠。
 */
#include <fyxos/pipe.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/sched.h>
#include <fyxos/sync.h>

#define PIPE_CAPACITY 512u

struct fyx_pipe {
    spinlock_t lock;
    u32        readers;
    u32        writers;
    u32        read_pos;
    u32        write_pos;
    u32        count;
    u8         data[PIPE_CAPACITY];
};

static struct fyx_pipe *fd_pipe(struct process *p, int fd, u8 required)
{
    if (!p || fd < 0 || fd >= PROC_MAX_FDS)
        return NULL;
    struct fyx_fd *f = &p->fds[fd];
    if (!f->pipe || (f->flags & required) != required)
        return NULL;
    return f->pipe;
}

static void pipe_release_endpoint(struct fyx_pipe *pipe, u8 flags)
{
    if (!pipe)
        return;

    u64 irq = irq_save_disable();
    spin_lock(&pipe->lock);
    if (flags & FYX_FD_READ)
        pipe->readers--;
    if (flags & FYX_FD_WRITE)
        pipe->writers--;
    bool reclaim = pipe->readers == 0 && pipe->writers == 0;
    spin_unlock(&pipe->lock);
    irq_restore(irq);

    if (reclaim)
        kfree(pipe);
}

long sys_pipe(int *fds)
{
    struct process *p = current_process();
    if (!p || !fds)
        return -FYX_EFAULT;

    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i].pipe)
            continue;
        if (read_fd < 0)
            read_fd = i;
        else {
            write_fd = i;
            break;
        }
    }
    if (read_fd < 0 || write_fd < 0)
        return -FYX_ENOMEM;

    struct fyx_pipe *pipe = kzalloc(sizeof(*pipe));
    if (!pipe)
        return -FYX_ENOMEM;
    spinlock_init(&pipe->lock, "pipe");
    pipe->readers = 1;
    pipe->writers = 1;

    p->fds[read_fd].pipe = pipe;
    p->fds[read_fd].flags = FYX_FD_READ;
    p->fds[write_fd].pipe = pipe;
    p->fds[write_fd].flags = FYX_FD_WRITE;

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

long sys_close(int fd)
{
    struct process *p = current_process();
    if (!p || fd < 0 || fd >= PROC_MAX_FDS || !p->fds[fd].pipe)
        return -FYX_EINVAL;

    struct fyx_pipe *pipe = p->fds[fd].pipe;
    u8 flags = p->fds[fd].flags;
    p->fds[fd].pipe = NULL;
    p->fds[fd].flags = 0;
    pipe_release_endpoint(pipe, flags);
    return 0;
}

void pipe_close_all(struct process *p)
{
    if (!p)
        return;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!p->fds[i].pipe)
            continue;
        struct fyx_pipe *pipe = p->fds[i].pipe;
        u8 flags = p->fds[i].flags;
        p->fds[i].pipe = NULL;
        p->fds[i].flags = 0;
        pipe_release_endpoint(pipe, flags);
    }
}

void pipe_clone_fds(struct process *child, const struct process *parent)
{
    if (!child || !parent)
        return;

    for (int i = 0; i < PROC_MAX_FDS; i++) {
        struct fyx_pipe *pipe = parent->fds[i].pipe;
        if (!pipe)
            continue;
        u8 flags = parent->fds[i].flags;
        child->fds[i].pipe = pipe;
        child->fds[i].flags = flags;

        u64 irq = irq_save_disable();
        spin_lock(&pipe->lock);
        if (flags & FYX_FD_READ)
            pipe->readers++;
        if (flags & FYX_FD_WRITE)
            pipe->writers++;
        spin_unlock(&pipe->lock);
        irq_restore(irq);
    }
}

long sys_fd_read(int fd, void *buf, size_t len)
{
    struct process *p = current_process();
    struct fyx_pipe *pipe = fd_pipe(p, fd, FYX_FD_READ);
    if (!pipe)
        return -FYX_EINVAL;
    if (len == 0)
        return 0;
    if (!buf)
        return -FYX_EFAULT;

    for (;;) {
        u64 irq = irq_save_disable();
        spin_lock(&pipe->lock);

        if (pipe->count != 0) {
            size_t n = MIN((size_t)pipe->count, len);
            for (size_t i = 0; i < n; i++) {
                ((u8 *)buf)[i] = pipe->data[pipe->read_pos];
                pipe->read_pos = (pipe->read_pos + 1) % PIPE_CAPACITY;
            }
            pipe->count -= n;
            spin_unlock(&pipe->lock);
            irq_restore(irq);
            return (long)n;
        }

        bool eof = pipe->writers == 0;
        spin_unlock(&pipe->lock);
        irq_restore(irq);
        if (eof)
            return 0;

        /* 尚无数据：真正让出 CPU，写端才能运行 */
        thread_sleep_ms(1);
    }
}

long sys_fd_write(int fd, const void *buf, size_t len)
{
    struct process *p = current_process();
    struct fyx_pipe *pipe = fd_pipe(p, fd, FYX_FD_WRITE);
    if (!pipe)
        return -FYX_EINVAL;
    if (len == 0)
        return 0;
    if (!buf)
        return -FYX_EFAULT;

    size_t done = 0;
    while (done < len) {
        u64 irq = irq_save_disable();
        spin_lock(&pipe->lock);

        if (pipe->readers == 0) {
            spin_unlock(&pipe->lock);
            irq_restore(irq);
            return done ? (long)done : -FYX_EPIPE;
        }

        size_t space = PIPE_CAPACITY - pipe->count;
        if (space != 0) {
            size_t n = MIN(space, len - done);
            for (size_t i = 0; i < n; i++) {
                pipe->data[pipe->write_pos] = ((const u8 *)buf)[done + i];
                pipe->write_pos = (pipe->write_pos + 1) % PIPE_CAPACITY;
            }
            pipe->count += n;
            done += n;
            spin_unlock(&pipe->lock);
            irq_restore(irq);
            continue;
        }

        spin_unlock(&pipe->lock);
        irq_restore(irq);
        /* 缓冲区满：等读端腾出空间 */
        thread_sleep_ms(1);
    }
    return (long)done;
}
