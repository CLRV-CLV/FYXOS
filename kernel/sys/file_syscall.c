/* FYXOS — 文件相关系统调用 */
#include <fyxos/syscall.h>
#include <fyxos/process.h>
#include <fyxos/pipe.h>
#include <fyxos/vfs.h>

long sys_open(const char *path, u32 flags, u32 mode)
{
    UNUSED(mode);                       /* v1 尚无 UNIX 权限位 */
    struct process *p = current_process();
    if (!p || !path)
        return -FYX_EFAULT;
    struct fyx_file *file;
    int rc = vfs_open(path, flags, &file);
    if (rc != 0)
        return rc;
    for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
        if (p->fds[fd].type != FYX_FD_NONE)
            continue;
        p->fds[fd].file = file;
        p->fds[fd].type = FYX_FD_FILE;
        p->fds[fd].flags = FYX_FD_READ | FYX_FD_WRITE;
        return fd;
    }
    vfs_file_close(file);
    return -FYX_ENOSPC;
}

long sys_lseek(int fd, i64 offset, u32 whence)
{
    struct process *p = current_process();
    if (!p || fd < 0 || fd >= PROC_MAX_FDS ||
        p->fds[fd].type != FYX_FD_FILE)
        return -FYX_EBADF;
    return vfs_file_seek(p->fds[fd].file, offset, whence);
}

long sys_stat(const char *path, void *out)
{
    if (!path || !out)
        return -FYX_EFAULT;
    return vfs_stat(path, out);
}

long sys_mkdir(const char *path, u32 mode)
{
    UNUSED(mode);
    if (!path)
        return -FYX_EFAULT;
    return vfs_mkdir(path);
}

long sys_unlink(const char *path)
{
    if (!path)
        return -FYX_EFAULT;
    return vfs_unlink(path);
}

long sys_sync(void)
{
    return vfs_sync();
}
