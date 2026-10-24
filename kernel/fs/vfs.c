/*
 * FYXOS — VFS 文件对象层
 *
 * 路径解析和 CoW 持久化由 FYXFS 后端完成；这里统一打开文件的生命周期、
 * 权限、共享偏移与 seek 语义。进程 fd 和救援 Shell 都走这一套接口。
 */
#include <fyxos/vfs.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>

static u32 g_open_files;

int vfs_mount(u64 partition_lba, u64 partition_sectors)
{
    if (g_open_files)
        return -FYX_EBUSY;
    return fyxfs_mount(partition_lba, partition_sectors);
}

int vfs_format(u64 partition_lba, u64 partition_sectors)
{
    if (fyxfs_mounted() || g_open_files)
        return -FYX_EBUSY;
    return fyxfs_format(partition_lba, partition_sectors);
}

int vfs_unmount(void)
{
    if (g_open_files)
        return -FYX_EBUSY;
    return fyxfs_unmount();
}

int vfs_sync(void)
{
    return fyxfs_sync();
}

bool vfs_mounted(void)
{
    return fyxfs_mounted();
}

int vfs_lookup(const char *path, u32 *node)
{
    return fyxfs_lookup(path, node);
}

int vfs_stat(const char *path, struct vfs_stat *out)
{
    if (!out)
        return -FYX_EINVAL;
    u32 node;
    int rc = fyxfs_lookup(path, &node);
    if (rc != 0)
        return rc;
    struct fyxfs_node_info info;
    rc = fyxfs_node_info(node, &info);
    if (rc != 0)
        return rc;
    out->size = info.size;
    out->node = info.node;
    out->type = info.type;
    out->flags = info.flags;
    out->crc32 = info.crc32;
    return FYX_OK;
}

int vfs_mkdir(const char *path)
{
    return fyxfs_create(path, FYXFS_NODE_DIR, NULL);
}

int vfs_create(const char *path)
{
    return fyxfs_create(path, FYXFS_NODE_FILE, NULL);
}

int vfs_unlink(const char *path)
{
    return fyxfs_unlink(path);
}

int vfs_rename(const char *old_path, const char *new_path)
{
    return fyxfs_rename(old_path, new_path);
}

int vfs_getdents(const char *path, u32 index, struct fyxfs_node_info *out)
{
    u32 dir;
    int rc = fyxfs_lookup(path, &dir);
    if (rc != 0)
        return rc;
    return fyxfs_getdents(dir, index, out);
}

int vfs_open(const char *path, u32 flags, struct fyx_file **out)
{
    if (!path || !out || (flags & VFS_O_ACCMODE) > VFS_O_RDWR)
        return -FYX_EINVAL;
    *out = NULL;
    u32 node;
    int rc = fyxfs_lookup(path, &node);
    if (rc == -FYX_ENOENT && (flags & VFS_O_CREAT)) {
        rc = fyxfs_create(path, FYXFS_NODE_FILE, &node);
    }
    if (rc != 0)
        return rc;

    struct fyxfs_node_info info;
    rc = fyxfs_node_info(node, &info);
    if (rc != 0)
        return rc;
    if ((flags & VFS_O_TRUNC) && (flags & VFS_O_ACCMODE) != VFS_O_RDONLY) {
        rc = fyxfs_truncate_node(node);
        if (rc != 0)
            return rc;
        info.size = 0;
    }

    struct fyx_file *file = kzalloc(sizeof(*file));
    if (!file)
        return -FYX_ENOMEM;
    file->node = node;
    file->refs = 1;
    file->flags = flags;
    file->type = info.type;
    file->offset = (flags & VFS_O_APPEND) ? info.size : 0;
    g_open_files++;
    *out = file;
    return FYX_OK;
}

void vfs_file_ref(struct fyx_file *file)
{
    if (file)
        file->refs++;
}

int vfs_file_close(struct fyx_file *file)
{
    if (!file || file->refs == 0)
        return -FYX_EBADF;
    file->refs--;
    if (file->refs == 0) {
        if (g_open_files)
            g_open_files--;
        kfree(file);
    }
    return FYX_OK;
}

long vfs_file_read(struct fyx_file *file, void *buf, size_t len)
{
    if (!file || !buf)
        return -FYX_EFAULT;
    if ((file->flags & VFS_O_ACCMODE) == VFS_O_WRONLY)
        return -FYX_EBADF;
    size_t done = 0;
    int rc = fyxfs_read_node(file->node, file->offset, buf, len, &done);
    if (rc != 0)
        return rc;
    file->offset += done;
    return (long)done;
}

long vfs_file_write(struct fyx_file *file, const void *buf, size_t len)
{
    if (!file || !buf)
        return -FYX_EFAULT;
    if ((file->flags & VFS_O_ACCMODE) == VFS_O_RDONLY)
        return -FYX_EBADF;
    if (file->flags & VFS_O_APPEND) {
        struct fyxfs_node_info info;
        int rc = fyxfs_node_info(file->node, &info);
        if (rc != 0)
            return rc;
        file->offset = info.size;
    }
    size_t done = 0;
    int rc = fyxfs_write_node(file->node, file->offset, buf, len, &done);
    if (rc != 0)
        return rc;
    file->offset += done;
    return (long)done;
}

long vfs_file_seek(struct fyx_file *file, i64 offset, u32 whence)
{
    if (!file)
        return -FYX_EBADF;
    i64 base;
    if (whence == VFS_SEEK_SET)
        base = 0;
    else if (whence == VFS_SEEK_CUR)
        base = (i64)file->offset;
    else if (whence == VFS_SEEK_END) {
        struct fyxfs_node_info info;
        int rc = fyxfs_node_info(file->node, &info);
        if (rc != 0)
            return rc;
        base = (i64)info.size;
    } else {
        return -FYX_EINVAL;
    }
    if ((offset > 0 && base > (i64)(~0ull >> 1) - offset) ||
        (offset < 0 && base < (-(i64)(~0ull >> 1) - 1) - offset))
        return -FYX_ERANGE;
    i64 target = base + offset;
    if (target < 0)
        return -FYX_EINVAL;
    file->offset = (u64)target;
    return (long)file->offset;
}

int vfs_write_file(const char *path, const void *buf, size_t len)
{
    struct fyx_file *file;
    int rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT, &file);
    if (rc != 0)
        return rc;
    /* 不先执行独立的 O_TRUNC 事务：数据、新长度和根指针一次发布。 */
    rc = fyxfs_replace_node(file->node, buf, len);
    int close_rc = vfs_file_close(file);
    return rc != 0 ? rc : close_rc;
}

int vfs_read_file(const char *path, void **out, size_t *size)
{
    if (!out || !size)
        return -FYX_EINVAL;
    *out = NULL;
    *size = 0;
    struct fyx_file *file;
    int rc = vfs_open(path, VFS_O_RDONLY, &file);
    if (rc != 0)
        return rc;
    struct fyxfs_node_info info;
    rc = fyxfs_node_info(file->node, &info);
    if (rc != 0 || info.type != FYXFS_NODE_FILE || info.size > 0x7FFFFFFFull) {
        vfs_file_close(file);
        return rc != 0 ? rc : -FYX_EINVAL;
    }
    u8 *data = kmalloc(info.size ? (size_t)info.size : 1);
    if (!data) {
        vfs_file_close(file);
        return -FYX_ENOMEM;
    }
    size_t done = 0;
    while (done < info.size) {
        long n = vfs_file_read(file, data + done, (size_t)info.size - done);
        if (n <= 0) {
            kfree(data);
            vfs_file_close(file);
            return n < 0 ? (int)n : -FYX_EIO;
        }
        done += (size_t)n;
    }
    vfs_file_close(file);
    *out = data;
    *size = (size_t)info.size;
    return FYX_OK;
}

int vfs_snapshot_create(const char *name, u64 *id)
{
    return fyxfs_snapshot_create(name, id);
}

int vfs_snapshot_get(u32 index, struct fyxfs_snapshot_info *out)
{
    return fyxfs_snapshot_get(index, out);
}

int vfs_snapshot_rollback(u64 id, bool preserve_home)
{
    if (g_open_files)
        return -FYX_EBUSY;
    return fyxfs_snapshot_rollback(id, preserve_home);
}

int vfs_check(bool repair, u32 *errors)
{
    return fyxfs_check(repair, errors);
}

void vfs_stats(struct fyxfs_stats *out)
{
    fyxfs_get_stats(out);
}
