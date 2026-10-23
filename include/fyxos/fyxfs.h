/*
 * FYXOS — FYXFS CoW 文件系统接口
 *
 * 磁盘格式与路径操作在这里实现；VFS 只依赖节点号和下列接口。所有数据块
 * 与根块一旦提交就不可改写，更新通过新根 + 双超级块原子发布。
 */
#ifndef FYXOS_FYXFS_H
#define FYXOS_FYXFS_H

#include <fyxos/types.h>

#define FYXFS_BLOCK_SIZE   4096u
#define FYXFS_NAME_MAX     23u
#define FYXFS_MAX_NODES    40u
#define FYXFS_MAX_SNAPSHOTS 16u

#define FYXFS_NODE_FILE    1u
#define FYXFS_NODE_DIR     2u

struct fyxfs_node_info {
    u32 node;
    u32 parent;
    u32 type;
    u32 flags;
    u64 size;
    u32 crc32;
    char name[FYXFS_NAME_MAX + 1];
};

struct fyxfs_snapshot_info {
    u64 id;
    u64 generation;
    u32 root_block;
    u32 flags;
    char name[FYXFS_NAME_MAX + 1];
};

struct fyxfs_stats {
    u64 partition_lba;
    u64 partition_sectors;
    u64 generation;
    u32 total_blocks;
    u32 next_block;
    u32 active_root;
    u32 nodes;
    u32 snapshots;
};

int fyxfs_format(u64 partition_lba, u64 partition_sectors);
int fyxfs_mount(u64 partition_lba, u64 partition_sectors);
int fyxfs_unmount(void);
int fyxfs_sync(void);
bool fyxfs_mounted(void);

int fyxfs_lookup(const char *path, u32 *node);
int fyxfs_node_info(u32 node, struct fyxfs_node_info *out);
int fyxfs_find_child(u32 parent, const char *name, u32 *node);
int fyxfs_create(const char *path, u32 type, u32 *node);
int fyxfs_unlink(const char *path);
int fyxfs_read_node(u32 node, u64 offset, void *buf, size_t len,
                    size_t *done);
int fyxfs_write_node(u32 node, u64 offset, const void *buf, size_t len,
                     size_t *done);
int fyxfs_replace_node(u32 node, const void *buf, size_t len);
int fyxfs_truncate_node(u32 node);
int fyxfs_getdents(u32 dir, u32 index, struct fyxfs_node_info *out);

int fyxfs_snapshot_create(const char *name, u64 *id);
int fyxfs_snapshot_get(u32 index, struct fyxfs_snapshot_info *out);
int fyxfs_snapshot_rollback(u64 id, bool preserve_home);
int fyxfs_check(bool repair, u32 *errors);
void fyxfs_get_stats(struct fyxfs_stats *out);

u32 fyx_crc32(const void *data, size_t len);

#endif /* FYXOS_FYXFS_H */
