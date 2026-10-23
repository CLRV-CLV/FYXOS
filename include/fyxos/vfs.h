/*
 * FYXOS — 虚拟文件系统与打开文件对象
 */
#ifndef FYXOS_VFS_H
#define FYXOS_VFS_H

#include <fyxos/types.h>
#include <fyxos/fyxfs.h>

#define VFS_PATH_MAX 128u

#define VFS_O_RDONLY 0x00u
#define VFS_O_WRONLY 0x01u
#define VFS_O_RDWR   0x02u
#define VFS_O_ACCMODE 0x03u
#define VFS_O_CREAT  0x10u
#define VFS_O_TRUNC  0x20u
#define VFS_O_APPEND 0x40u

#define VFS_SEEK_SET 0u
#define VFS_SEEK_CUR 1u
#define VFS_SEEK_END 2u

struct fyx_file {
    u32 node;
    u32 refs;
    u32 flags;
    u32 type;
    u64 offset;
};

struct vfs_stat {
    u64 size;
    u32 node;
    u32 type;
    u32 flags;
    u32 crc32;
};

int  vfs_mount(u64 partition_lba, u64 partition_sectors);
int  vfs_format(u64 partition_lba, u64 partition_sectors);
int  vfs_unmount(void);
int  vfs_sync(void);
bool vfs_mounted(void);

int  vfs_lookup(const char *path, u32 *node);
int  vfs_stat(const char *path, struct vfs_stat *out);
int  vfs_mkdir(const char *path);
int  vfs_create(const char *path);
int  vfs_unlink(const char *path);
int  vfs_getdents(const char *path, u32 index,
                  struct fyxfs_node_info *out);

int  vfs_open(const char *path, u32 flags, struct fyx_file **out);
void vfs_file_ref(struct fyx_file *file);
int  vfs_file_close(struct fyx_file *file);
long vfs_file_read(struct fyx_file *file, void *buf, size_t len);
long vfs_file_write(struct fyx_file *file, const void *buf, size_t len);
long vfs_file_seek(struct fyx_file *file, i64 offset, u32 whence);

/* 内核启动安装器与救援 Shell 使用的便捷接口。 */
int  vfs_write_file(const char *path, const void *buf, size_t len);
int  vfs_read_file(const char *path, void **out, size_t *size);

int  vfs_snapshot_create(const char *name, u64 *id);
int  vfs_snapshot_get(u32 index, struct fyxfs_snapshot_info *out);
int  vfs_snapshot_rollback(u64 id, bool preserve_home);
int  vfs_check(bool repair, u32 *errors);
void vfs_stats(struct fyxfs_stats *out);

#endif /* FYXOS_VFS_H */
