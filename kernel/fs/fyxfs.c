/*
 * FYXOS — FYXFS v1：写时复制、校验和、双超级块事务
 *
 * 磁盘布局（相对于分区起点）：
 *   sector 0/1       两个独立超级块，按 generation 选择最新有效者
 *   sector 2..15     保留
 *   sector 16..      4 KiB 块区，块 0 起
 *
 * 文件数据、目录元数据与快照根都不可原地修改。一次写入先分配并写新数据，
 * 再写新的根块，flush 后才写另一个超级块。任何较早的断电点都只会留下不
 * 可达的新块；超级块发布后才可能对挂载者可见。快照仅记录一个旧根块号，
 * 因而创建为 O(1)，回滚则通过一次新的 CoW 提交完成。
 *
 * v1 的刻意边界：固定 40 个节点、16 个快照、单分区、追加分配，尚未实现
 * 空闲块回收、压缩、去重与 B 树索引。这些限制会明确报告 ENOSPC，绝不会
 * 通过覆盖快照仍引用的数据来“腾出空间”。
 */
#include <fyxos/fyxfs.h>
#include <fyxos/block.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>

#define FYXFS_VERSION       1u
#define FYXFS_SUPER_SECTORS 16u
#define FYXFS_EXTENTS       4u
#define FYXFS_NODE_USED     0x0001u
#define FYXFS_PATH_CAP      128u

static const char g_super_magic[8] = { 'F','Y','X','F','S','1',0,0 };
static const char g_root_magic[8]  = { 'F','Y','X','R','O','O','T','1' };

struct fyxfs_extent_disk {
    u32 start;
    u32 blocks;
} PACKED;

struct fyxfs_node_disk {
    u64 size;
    u32 parent;
    u32 crc32;
    u16 flags;
    u8  type;
    u8  extent_count;
    char name[FYXFS_NAME_MAX + 1];
    struct fyxfs_extent_disk extents[FYXFS_EXTENTS];
} PACKED;

struct fyxfs_snapshot_disk {
    u64 id;
    u64 generation;
    u32 root_block;
    u32 flags;
    char name[FYXFS_NAME_MAX + 1];
} PACKED;

struct fyxfs_root_disk {
    char magic[8];
    u32 version;
    u32 reserved;
    u64 generation;
    u32 node_count;
    u32 snapshot_count;
    struct fyxfs_node_disk nodes[FYXFS_MAX_NODES];
    struct fyxfs_snapshot_disk snapshots[FYXFS_MAX_SNAPSHOTS];
} PACKED;

union fyxfs_root_block {
    struct fyxfs_root_disk root;
    u8 bytes[FYXFS_BLOCK_SIZE];
};

struct fyxfs_super_disk {
    char magic[8];
    u32 version;
    u32 block_size;
    u64 partition_lba;
    u64 partition_sectors;
    u64 generation;
    u32 root_block;
    u32 next_block;
    u32 slot;
    u32 reserved;
} PACKED;

union fyxfs_super_block {
    struct fyxfs_super_disk super;
    u8 bytes[BLOCK_SECTOR_SIZE];
};

typedef char fyxfs_root_size_check[
    sizeof(struct fyxfs_root_disk) <= FYXFS_BLOCK_SIZE - 4 ? 1 : -1];
typedef char fyxfs_super_size_check[
    sizeof(struct fyxfs_super_disk) <= BLOCK_SECTOR_SIZE - 4 ? 1 : -1];

static struct {
    bool mounted;
    u64 partition_lba;
    u64 partition_sectors;
    u32 total_blocks;
    u32 next_block;
    u32 active_root;
    u32 active_slot;
    union fyxfs_root_block root;
} g_fs;

/* ------------------------------- CRC32 ---------------------------------- */
static u32 crc32_update(u32 crc, const void *data, size_t len)
{
    const u8 *p = data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)-(i32)(crc & 1));
    }
    return crc;
}

u32 fyx_crc32(const void *data, size_t len)
{
    return ~crc32_update(0xFFFFFFFFu, data, len);
}

/* ----------------------------- 原始块 I/O -------------------------------- */
static u64 block_lba(u32 block)
{
    return g_fs.partition_lba + FYXFS_SUPER_SECTORS +
           (u64)block * (FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
}

static u64 block_lba_for(u64 part_lba, u32 block)
{
    return part_lba + FYXFS_SUPER_SECTORS +
           (u64)block * (FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
}

static int alloc_blocks(u32 count, u32 *start)
{
    if (!g_fs.mounted || count == 0 || g_fs.next_block > g_fs.total_blocks ||
        count > g_fs.total_blocks - g_fs.next_block)
        return -FYX_ENOSPC;
    *start = g_fs.next_block;
    g_fs.next_block += count;
    return FYX_OK;
}

static void super_checksum(union fyxfs_super_block *sb)
{
    u32 *crc = (u32 *)&sb->bytes[BLOCK_SECTOR_SIZE - 4];
    *crc = 0;
    *crc = fyx_crc32(sb->bytes, BLOCK_SECTOR_SIZE - 4);
}

static void root_checksum(union fyxfs_root_block *rb)
{
    u32 *crc = (u32 *)&rb->bytes[FYXFS_BLOCK_SIZE - 4];
    *crc = 0;
    *crc = fyx_crc32(rb->bytes, FYXFS_BLOCK_SIZE - 4);
}

static bool super_valid(const union fyxfs_super_block *sb,
                        u64 part_lba, u64 part_sectors, u32 total_blocks)
{
    const struct fyxfs_super_disk *s = &sb->super;
    if (memcmp(s->magic, g_super_magic, 8) != 0 ||
        s->version != FYXFS_VERSION || s->block_size != FYXFS_BLOCK_SIZE ||
        s->partition_lba != part_lba || s->partition_sectors != part_sectors)
        return false;
    u32 stored;
    memcpy(&stored, &sb->bytes[BLOCK_SECTOR_SIZE - 4], 4);
    if (fyx_crc32(sb->bytes, BLOCK_SECTOR_SIZE - 4) != stored)
        return false;
    return s->root_block < s->next_block && s->next_block <= total_blocks;
}

static int read_root_at(u64 part_lba, u32 block, u32 total_blocks,
                        union fyxfs_root_block *out)
{
    if (block >= total_blocks)
        return -FYX_EIO;
    int rc = block_read(block_lba_for(part_lba, block), out->bytes,
                        FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    if (rc != 0)
        return rc;
    if (memcmp(out->root.magic, g_root_magic, 8) != 0 ||
        out->root.version != FYXFS_VERSION ||
        out->root.node_count > FYXFS_MAX_NODES ||
        out->root.snapshot_count > FYXFS_MAX_SNAPSHOTS)
        return -FYX_EIO;
    u32 stored;
    memcpy(&stored, &out->bytes[FYXFS_BLOCK_SIZE - 4], 4);
    if (fyx_crc32(out->bytes, FYXFS_BLOCK_SIZE - 4) != stored)
        return -FYX_EIO;
    return FYX_OK;
}

/* ------------------------------- 格式化 --------------------------------- */
int fyxfs_format(u64 partition_lba, u64 partition_sectors)
{
    if (partition_sectors <= FYXFS_SUPER_SECTORS)
        return -FYX_ENOSPC;
    u64 blocks = (partition_sectors - FYXFS_SUPER_SECTORS) /
                 (FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    if (blocks < 16 || blocks > 0xFFFFFFFFu)
        return -FYX_ENOSPC;

    /* 格式化是显式破坏性操作：先作废旧超级块，避免半格式化仍挂到旧根。 */
    u8 zero[BLOCK_SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));
    for (u32 slot = 0; slot < 2; slot++) {
        int rc = block_write(partition_lba + slot, zero, 1);
        if (rc != 0)
            return rc;
    }
    int rc = block_flush();
    if (rc != 0)
        return rc;

    union fyxfs_root_block *rb = kzalloc(sizeof(*rb));
    if (!rb)
        return -FYX_ENOMEM;
    memcpy(rb->root.magic, g_root_magic, 8);
    rb->root.version = FYXFS_VERSION;
    rb->root.generation = 1;
    rb->root.node_count = 1;
    rb->root.nodes[0].flags = FYXFS_NODE_USED;
    rb->root.nodes[0].type = FYXFS_NODE_DIR;
    rb->root.nodes[0].parent = 0;
    rb->root.nodes[0].name[0] = '/';
    root_checksum(rb);
    rc = block_write(block_lba_for(partition_lba, 0), rb->bytes,
                     FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    kfree(rb);
    if (rc != 0)
        return rc;
    rc = block_flush();
    if (rc != 0)
        return rc;

    for (u32 slot = 0; slot < 2; slot++) {
        union fyxfs_super_block sb;
        memset(&sb, 0, sizeof(sb));
        memcpy(sb.super.magic, g_super_magic, 8);
        sb.super.version = FYXFS_VERSION;
        sb.super.block_size = FYXFS_BLOCK_SIZE;
        sb.super.partition_lba = partition_lba;
        sb.super.partition_sectors = partition_sectors;
        sb.super.generation = 1;
        sb.super.root_block = 0;
        sb.super.next_block = 1;
        sb.super.slot = slot;
        super_checksum(&sb);
        rc = block_write(partition_lba + slot, sb.bytes, 1);
        if (rc != 0)
            return rc;
    }
    rc = block_flush();
    if (rc == 0)
        kprintf("[fyxfs] 格式化完成：%llu 个 4KiB 块\n",
                (unsigned long long)blocks);
    return rc;
}

/* ------------------------------- 挂载 ----------------------------------- */
int fyxfs_mount(u64 partition_lba, u64 partition_sectors)
{
    if (g_fs.mounted)
        return -FYX_EBUSY;
    if (partition_sectors <= FYXFS_SUPER_SECTORS)
        return -FYX_EINVAL;
    u64 blocks64 = (partition_sectors - FYXFS_SUPER_SECTORS) /
                   (FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    if (blocks64 < 16 || blocks64 > 0xFFFFFFFFu)
        return -FYX_EINVAL;
    u32 total_blocks = (u32)blocks64;

    union fyxfs_super_block sb[2];
    bool valid[2] = { false, false };
    bool saw_magic = false;
    for (u32 slot = 0; slot < 2; slot++) {
        memset(&sb[slot], 0, sizeof(sb[slot]));
        int rc = block_read(partition_lba + slot, sb[slot].bytes, 1);
        if (rc != 0)
            return rc;
        if (memcmp(sb[slot].super.magic, g_super_magic, 8) == 0)
            saw_magic = true;
        valid[slot] = super_valid(&sb[slot], partition_lba,
                                  partition_sectors, total_blocks);
    }
    if (!valid[0] && !valid[1])
        return saw_magic ? -FYX_EIO : -FYX_ENOENT;

    u32 slot;
    if (valid[0] && valid[1])
        slot = sb[1].super.generation >= sb[0].super.generation ? 1 : 0;
    else
        slot = valid[1] ? 1 : 0;

    union fyxfs_root_block *root = kzalloc(sizeof(*root));
    if (!root)
        return -FYX_ENOMEM;
    int rc = read_root_at(partition_lba, sb[slot].super.root_block,
                          total_blocks, root);
    if (rc != 0 || root->root.generation != sb[slot].super.generation) {
        /* 最新超级块可能正好在断电前发布了一半，回退到另一份完整事务。 */
        u32 other = slot ^ 1;
        if (valid[other]) {
            rc = read_root_at(partition_lba, sb[other].super.root_block,
                              total_blocks, root);
            if (rc == 0 && root->root.generation == sb[other].super.generation)
                slot = other;
            else
                rc = -FYX_EIO;
        }
        if (rc != 0) {
            kfree(root);
            return -FYX_EIO;
        }
    }

    memset(&g_fs, 0, sizeof(g_fs));
    g_fs.mounted = true;
    g_fs.partition_lba = partition_lba;
    g_fs.partition_sectors = partition_sectors;
    g_fs.total_blocks = total_blocks;
    g_fs.next_block = sb[slot].super.next_block;
    g_fs.active_root = sb[slot].super.root_block;
    g_fs.active_slot = slot;
    memcpy(&g_fs.root, root, sizeof(*root));
    kfree(root);
    kprintf("[fyxfs] 挂载成功：generation=%llu root=%u next=%u/%u slot=%u\n",
            (unsigned long long)g_fs.root.root.generation,
            g_fs.active_root, g_fs.next_block, g_fs.total_blocks,
            g_fs.active_slot);
    return FYX_OK;
}

int fyxfs_unmount(void)
{
    if (!g_fs.mounted)
        return -FYX_EINVAL;
    int rc = block_flush();
    if (rc == 0) {
        memset(&g_fs, 0, sizeof(g_fs));
        kprintf("[fyxfs] 已卸载\n");
    }
    return rc;
}

int fyxfs_sync(void)
{
    return g_fs.mounted ? block_flush() : -FYX_EINVAL;
}

bool fyxfs_mounted(void)
{
    return g_fs.mounted;
}

/* ------------------------------ 提交事务 --------------------------------- */
static int commit_root(const struct fyxfs_root_disk *new_root)
{
    if (!g_fs.mounted)
        return -FYX_EINVAL;
    u32 block;
    int rc = alloc_blocks(1, &block);
    if (rc != 0)
        return rc;

    union fyxfs_root_block *rb = kzalloc(sizeof(*rb));
    if (!rb)
        return -FYX_ENOMEM;
    rb->root = *new_root;
    rb->root.generation = g_fs.root.root.generation + 1;
    root_checksum(rb);
    rc = block_write(block_lba(block), rb->bytes,
                     FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    if (rc == 0)
        rc = block_flush();
    if (rc != 0) {
        kfree(rb);
        return rc;
    }

    u32 slot = g_fs.active_slot ^ 1;
    union fyxfs_super_block sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.super.magic, g_super_magic, 8);
    sb.super.version = FYXFS_VERSION;
    sb.super.block_size = FYXFS_BLOCK_SIZE;
    sb.super.partition_lba = g_fs.partition_lba;
    sb.super.partition_sectors = g_fs.partition_sectors;
    sb.super.generation = rb->root.generation;
    sb.super.root_block = block;
    sb.super.next_block = g_fs.next_block;
    sb.super.slot = slot;
    super_checksum(&sb);
    rc = block_write(g_fs.partition_lba + slot, sb.bytes, 1);
    if (rc == 0)
        rc = block_flush();
    if (rc != 0) {
        kfree(rb);
        return rc;
    }

    memcpy(&g_fs.root, rb, sizeof(g_fs.root));
    g_fs.active_root = block;
    g_fs.active_slot = slot;
    kfree(rb);
    return FYX_OK;
}

/* ------------------------------ 节点与路径 -------------------------------- */
static bool node_used(const struct fyxfs_root_disk *root, u32 node)
{
    return node < root->node_count && node < FYXFS_MAX_NODES &&
           (root->nodes[node].flags & FYXFS_NODE_USED);
}

static void fill_info(const struct fyxfs_root_disk *root, u32 node,
                      struct fyxfs_node_info *out)
{
    const struct fyxfs_node_disk *n = &root->nodes[node];
    memset(out, 0, sizeof(*out));
    out->node = node;
    out->parent = n->parent;
    out->type = n->type;
    out->flags = n->flags;
    out->size = n->size;
    out->crc32 = n->crc32;
    memcpy(out->name, n->name, sizeof(out->name));
    out->name[FYXFS_NAME_MAX] = '\0';
}

int fyxfs_node_info(u32 node, struct fyxfs_node_info *out)
{
    if (!g_fs.mounted || !out || !node_used(&g_fs.root.root, node))
        return -FYX_ENOENT;
    fill_info(&g_fs.root.root, node, out);
    return FYX_OK;
}

static int find_child_root(const struct fyxfs_root_disk *root, u32 parent,
                           const char *name, u32 *node)
{
    if (!node_used(root, parent) || root->nodes[parent].type != FYXFS_NODE_DIR)
        return -FYX_ENOTDIR;
    for (u32 i = 1; i < root->node_count; i++) {
        if (!node_used(root, i) || root->nodes[i].parent != parent)
            continue;
        if (strncmp(root->nodes[i].name, name, FYXFS_NAME_MAX + 1) == 0) {
            if (node)
                *node = i;
            return FYX_OK;
        }
    }
    return -FYX_ENOENT;
}

int fyxfs_find_child(u32 parent, const char *name, u32 *node)
{
    if (!g_fs.mounted || !name)
        return -FYX_EINVAL;
    return find_child_root(&g_fs.root.root, parent, name, node);
}

static int lookup_root(const struct fyxfs_root_disk *root, const char *path,
                       u32 *node)
{
    if (!path || !node || path[0] == '\0')
        return -FYX_EINVAL;
    u32 current = 0;
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = 0;
        return FYX_OK;
    }

    while (*p) {
        char component[FYXFS_NAME_MAX + 1];
        size_t n = 0;
        while (p[n] && p[n] != '/') {
            if (n >= FYXFS_NAME_MAX)
                return -FYX_ERANGE;
            component[n] = p[n];
            n++;
        }
        component[n] = '\0';
        p += n;
        while (*p == '/')
            p++;
        if (n == 0 || (n == 1 && component[0] == '.'))
            continue;
        if (n == 2 && component[0] == '.' && component[1] == '.') {
            current = root->nodes[current].parent;
            if (!node_used(root, current))
                return -FYX_EIO;
            continue;
        }
        int rc = find_child_root(root, current, component, &current);
        if (rc != 0)
            return rc;
        if (*p && root->nodes[current].type != FYXFS_NODE_DIR)
            return -FYX_ENOTDIR;
    }
    *node = current;
    return FYX_OK;
}

int fyxfs_lookup(const char *path, u32 *node)
{
    if (!g_fs.mounted)
        return -FYX_EINVAL;
    return lookup_root(&g_fs.root.root, path, node);
}

static int alloc_node(struct fyxfs_root_disk *root, u32 *node)
{
    for (u32 i = 1; i < root->node_count; i++) {
        if (!(root->nodes[i].flags & FYXFS_NODE_USED)) {
            *node = i;
            return FYX_OK;
        }
    }
    if (root->node_count >= FYXFS_MAX_NODES)
        return -FYX_ENOSPC;
    *node = root->node_count++;
    return FYX_OK;
}

int fyxfs_create(const char *path, u32 type, u32 *node)
{
    if (!g_fs.mounted || !path ||
        (type != FYXFS_NODE_FILE && type != FYXFS_NODE_DIR))
        return -FYX_EINVAL;
    size_t len = strlen(path);
    if (len == 0 || len >= FYXFS_PATH_CAP || path[len - 1] == '/')
        return -FYX_EINVAL;

    char parent_path[FYXFS_PATH_CAP];
    memcpy(parent_path, path, len + 1);
    char *slash = NULL;
    for (char *p = parent_path; *p; p++)
        if (*p == '/')
            slash = p;
    const char *name;
    char name_copy[FYXFS_NAME_MAX + 1];
    if (!slash) {
        memcpy(parent_path, "/", 2);
        name = path;
    } else {
        /* /bin 的 slash 同时也是 parent_path 的首字符；先把 basename
         * 取出来，再把父路径截成 "/"，否则会把 name 的 'b' 一并清掉。 */
        size_t base_len = strlen(slash + 1);
        if (base_len > FYXFS_NAME_MAX)
            return -FYX_ERANGE;
        memcpy(name_copy, slash + 1, base_len + 1);
        name = name_copy;
        if (slash == parent_path)
            slash[1] = '\0';
        else
            *slash = '\0';
    }
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > FYXFS_NAME_MAX ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -FYX_EINVAL;

    u32 parent;
    int rc = lookup_root(&g_fs.root.root, parent_path, &parent);
    if (rc != 0)
        return rc;
    if (find_child_root(&g_fs.root.root, parent, name, NULL) == FYX_OK)
        return -FYX_EEXIST;

    struct fyxfs_root_disk next = g_fs.root.root;
    u32 created;
    rc = alloc_node(&next, &created);
    if (rc != 0)
        return rc;
    struct fyxfs_node_disk *n = &next.nodes[created];
    memset(n, 0, sizeof(*n));
    n->flags = FYXFS_NODE_USED;
    n->type = (u8)type;
    n->parent = parent;
    memcpy(n->name, name, name_len + 1);
    rc = commit_root(&next);
    if (rc == 0 && node)
        *node = created;
    return rc;
}

int fyxfs_unlink(const char *path)
{
    u32 node;
    int rc = fyxfs_lookup(path, &node);
    if (rc != 0)
        return rc;
    if (node == 0)
        return -FYX_EBUSY;
    const struct fyxfs_node_disk *n = &g_fs.root.root.nodes[node];
    if (n->type == FYXFS_NODE_DIR) {
        for (u32 i = 1; i < g_fs.root.root.node_count; i++)
            if (node_used(&g_fs.root.root, i) &&
                g_fs.root.root.nodes[i].parent == node)
                return -FYX_ENOTEMPTY;
    }
    struct fyxfs_root_disk next = g_fs.root.root;
    memset(&next.nodes[node], 0, sizeof(next.nodes[node]));
    return commit_root(&next);
}

int fyxfs_rename(const char *old_path, const char *new_path)
{
    if (!g_fs.mounted || !old_path || !new_path)
        return -FYX_EINVAL;
    u32 node;
    int rc = lookup_root(&g_fs.root.root, old_path, &node);
    if (rc != 0)
        return rc;
    if (node == 0)
        return -FYX_EBUSY;
    size_t len = strlen(new_path);
    if (len == 0 || len >= FYXFS_PATH_CAP || new_path[len - 1] == '/')
        return -FYX_EINVAL;

    char parent_path[FYXFS_PATH_CAP];
    memcpy(parent_path, new_path, len + 1);
    char *slash = NULL;
    for (char *p = parent_path; *p; p++)
        if (*p == '/')
            slash = p;
    const char *name;
    char name_copy[FYXFS_NAME_MAX + 1];
    if (!slash) {
        memcpy(parent_path, "/", 2);
        name = new_path;
    } else {
        size_t base_len = strlen(slash + 1);
        if (base_len > FYXFS_NAME_MAX)
            return -FYX_ERANGE;
        memcpy(name_copy, slash + 1, base_len + 1);
        name = name_copy;
        if (slash == parent_path)
            slash[1] = '\0';
        else
            *slash = '\0';
    }
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > FYXFS_NAME_MAX ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -FYX_EINVAL;

    u32 parent;
    rc = lookup_root(&g_fs.root.root, parent_path, &parent);
    if (rc != 0)
        return rc;
    if (g_fs.root.root.nodes[parent].type != FYXFS_NODE_DIR)
        return -FYX_ENOTDIR;
    u32 existing;
    rc = find_child_root(&g_fs.root.root, parent, name, &existing);
    if (rc == 0)
        return existing == node ? FYX_OK : -FYX_EEXIST;
    if (rc != -FYX_ENOENT)
        return rc;

    /* 目录不能移动到自己或自己的后代中；损坏的父链也不能造成死循环。 */
    u32 cursor = parent;
    for (u32 steps = 0; cursor && steps < FYXFS_MAX_NODES; steps++) {
        if (cursor == node)
            return -FYX_EINVAL;
        if (!node_used(&g_fs.root.root, cursor))
            return -FYX_EIO;
        cursor = g_fs.root.root.nodes[cursor].parent;
    }
    if (cursor)
        return -FYX_EIO;
    struct fyxfs_root_disk next = g_fs.root.root;
    next.nodes[node].parent = parent;
    memset(next.nodes[node].name, 0, sizeof(next.nodes[node].name));
    memcpy(next.nodes[node].name, name, name_len + 1);
    return commit_root(&next);
}

int fyxfs_getdents(u32 dir, u32 index, struct fyxfs_node_info *out)
{
    if (!g_fs.mounted || !out || !node_used(&g_fs.root.root, dir))
        return -FYX_ENOENT;
    if (g_fs.root.root.nodes[dir].type != FYXFS_NODE_DIR)
        return -FYX_ENOTDIR;
    u32 seen = 0;
    for (u32 i = 1; i < g_fs.root.root.node_count; i++) {
        if (!node_used(&g_fs.root.root, i) ||
            g_fs.root.root.nodes[i].parent != dir)
            continue;
        if (seen++ == index) {
            fill_info(&g_fs.root.root, i, out);
            return FYX_OK;
        }
    }
    return -FYX_ENOENT;
}

/* ------------------------------- 文件 I/O --------------------------------- */
static int extent_for_block(const struct fyxfs_node_disk *node, u32 logical,
                            u32 *physical)
{
    u32 base = 0;
    for (u32 i = 0; i < node->extent_count && i < FYXFS_EXTENTS; i++) {
        u32 count = node->extents[i].blocks;
        if (logical >= base && logical - base < count) {
            *physical = node->extents[i].start + (logical - base);
            return FYX_OK;
        }
        base += count;
    }
    return -FYX_EIO;
}

int fyxfs_read_node(u32 node, u64 offset, void *buf, size_t len,
                    size_t *done)
{
    if (done)
        *done = 0;
    if (!g_fs.mounted || !buf || !node_used(&g_fs.root.root, node))
        return -FYX_ENOENT;
    const struct fyxfs_node_disk *n = &g_fs.root.root.nodes[node];
    if (n->type != FYXFS_NODE_FILE)
        return -FYX_EISDIR;
    if (offset > n->size)
        return FYX_OK;
    size_t want = MIN(len, (size_t)(n->size - offset));
    size_t copied = 0;
    u8 sector[BLOCK_SECTOR_SIZE];

    while (copied < want) {
        u64 pos = offset + copied;
        u32 logical = (u32)(pos / FYXFS_BLOCK_SIZE);
        u32 physical;
        int rc = extent_for_block(n, logical, &physical);
        if (rc != 0 || physical >= g_fs.total_blocks)
            return -FYX_EIO;
        u32 in_block = (u32)(pos % FYXFS_BLOCK_SIZE);
        u32 sector_index = in_block / BLOCK_SECTOR_SIZE;
        u32 in_sector = in_block % BLOCK_SECTOR_SIZE;
        rc = block_read(block_lba(physical) + sector_index, sector, 1);
        if (rc != 0)
            return rc;
        size_t chunk = MIN((size_t)(BLOCK_SECTOR_SIZE - in_sector),
                           want - copied);
        memcpy((u8 *)buf + copied, sector + in_sector, chunk);
        copied += chunk;
    }
    if (done)
        *done = copied;
    return FYX_OK;
}

static int write_node_common(u32 node, u64 offset, const void *buf, size_t len,
                             size_t *done, bool replace)
{
    if (done)
        *done = 0;
    if (!g_fs.mounted || !buf || !node_used(&g_fs.root.root, node))
        return -FYX_ENOENT;
    const struct fyxfs_node_disk *old = &g_fs.root.root.nodes[node];
    if (old->type != FYXFS_NODE_FILE)
        return -FYX_EISDIR;
    if (len == 0 && !replace)
        return FYX_OK;
    if ((replace && offset != 0) || (u64)len > ~0ull - offset)
        return -FYX_ERANGE;
    u64 end = offset + len;
    u64 new_size = replace ? (u64)len : MAX(old->size, end);
    if (new_size > (u64)g_fs.total_blocks * FYXFS_BLOCK_SIZE)
        return -FYX_ENOSPC;

    u8 *data = kzalloc(new_size ? (size_t)new_size : 1);
    if (!data)
        return -FYX_ENOMEM;
    if (!replace && old->size) {
        size_t got = 0;
        int rc = fyxfs_read_node(node, 0, data, (size_t)old->size, &got);
        if (rc != 0 || got != old->size) {
            kfree(data);
            return rc != 0 ? rc : -FYX_EIO;
        }
    }
    memcpy(data + offset, buf, len);

    u32 blocks = (u32)ALIGN_UP(new_size, FYXFS_BLOCK_SIZE) / FYXFS_BLOCK_SIZE;
    u32 start = 0;
    int rc = FYX_OK;
    if (blocks)
        rc = alloc_blocks(blocks, &start);
    if (rc != 0) {
        kfree(data);
        return rc;
    }
    u8 *zero_block = kzalloc(FYXFS_BLOCK_SIZE);
    if (!zero_block) {
        kfree(data);
        return -FYX_ENOMEM;
    }
    for (u32 i = 0; i < blocks; i++) {
        const u8 *source = data + (u64)i * FYXFS_BLOCK_SIZE;
        if (i + 1 == blocks && (new_size % FYXFS_BLOCK_SIZE)) {
            size_t tail = (size_t)(new_size % FYXFS_BLOCK_SIZE);
            memcpy(zero_block, source, tail);
            source = zero_block;
        }
        rc = block_write(block_lba(start + i), source,
                         FYXFS_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
        if (rc != 0)
            break;
    }
    kfree(zero_block);
    if (rc != 0) {
        kfree(data);
        return rc;
    }

    struct fyxfs_root_disk next = g_fs.root.root;
    struct fyxfs_node_disk *n = &next.nodes[node];
    n->size = new_size;
    n->crc32 = fyx_crc32(data, (size_t)new_size);
    n->extent_count = blocks ? 1 : 0;
    memset(n->extents, 0, sizeof(n->extents));
    n->extents[0].start = start;
    n->extents[0].blocks = blocks;
    rc = commit_root(&next);
    kfree(data);
    if (rc == 0 && done)
        *done = len;
    return rc;
}

int fyxfs_write_node(u32 node, u64 offset, const void *buf, size_t len,
                     size_t *done)
{
    return write_node_common(node, offset, buf, len, done, false);
}

int fyxfs_replace_node(u32 node, const void *buf, size_t len)
{
    size_t done = 0;
    int rc = write_node_common(node, 0, buf, len, &done, true);
    if (rc == 0 && done != len)
        return -FYX_EIO;
    return rc;
}

int fyxfs_truncate_node(u32 node)
{
    if (!g_fs.mounted || !node_used(&g_fs.root.root, node))
        return -FYX_ENOENT;
    if (g_fs.root.root.nodes[node].type != FYXFS_NODE_FILE)
        return -FYX_EISDIR;
    struct fyxfs_root_disk next = g_fs.root.root;
    next.nodes[node].size = 0;
    next.nodes[node].crc32 = 0;
    next.nodes[node].extent_count = 0;
    memset(next.nodes[node].extents, 0, sizeof(next.nodes[node].extents));
    return commit_root(&next);
}

/* -------------------------------- 快照 ----------------------------------- */
int fyxfs_snapshot_create(const char *name, u64 *id)
{
    if (!g_fs.mounted || !name || !*name || strlen(name) > FYXFS_NAME_MAX)
        return -FYX_EINVAL;
    if (g_fs.root.root.snapshot_count >= FYXFS_MAX_SNAPSHOTS)
        return -FYX_ENOSPC;
    struct fyxfs_root_disk next = g_fs.root.root;
    u64 new_id = 1;
    for (u32 i = 0; i < next.snapshot_count; i++)
        new_id = MAX(new_id, next.snapshots[i].id + 1);
    struct fyxfs_snapshot_disk *s = &next.snapshots[next.snapshot_count++];
    memset(s, 0, sizeof(*s));
    s->id = new_id;
    s->generation = g_fs.root.root.generation;
    s->root_block = g_fs.active_root;
    strncpy(s->name, name, FYXFS_NAME_MAX);
    int rc = commit_root(&next);
    if (rc == 0) {
        if (id)
            *id = new_id;
        kprintf("[snapshot] 创建 #%llu '%s' -> root=%u\n",
                (unsigned long long)new_id, name, s->root_block);
    }
    return rc;
}

int fyxfs_snapshot_get(u32 index, struct fyxfs_snapshot_info *out)
{
    if (!g_fs.mounted || !out || index >= g_fs.root.root.snapshot_count)
        return -FYX_ENOENT;
    const struct fyxfs_snapshot_disk *s = &g_fs.root.root.snapshots[index];
    memset(out, 0, sizeof(*out));
    out->id = s->id;
    out->generation = s->generation;
    out->root_block = s->root_block;
    out->flags = s->flags;
    memcpy(out->name, s->name, sizeof(out->name));
    out->name[FYXFS_NAME_MAX] = '\0';
    return FYX_OK;
}

static bool descendant_of(const struct fyxfs_root_disk *root, u32 node,
                          u32 ancestor)
{
    for (u32 depth = 0; depth < FYXFS_MAX_NODES; depth++) {
        if (node == ancestor)
            return true;
        if (node == 0 || !node_used(root, node))
            return false;
        node = root->nodes[node].parent;
    }
    return false;
}

/* 把当前 /home 子树合并进待恢复根；节点号在必要时重新分配并修正 parent。 */
static int merge_current_home(struct fyxfs_root_disk *target)
{
    const struct fyxfs_root_disk *current = &g_fs.root.root;
    u32 current_home, target_home;
    if (lookup_root(current, "/home", &current_home) != FYX_OK)
        return FYX_OK;

    if (lookup_root(target, "/home", &target_home) == FYX_OK) {
        /* 必须先根据原树计算完整集合，再统一清理。边遍历边删父节点会
         * 让更深层后代在 descendant_of() 中失去到 /home 的路径。 */
        bool remove[FYXFS_MAX_NODES] = { false };
        for (u32 i = 1; i < target->node_count; i++)
            remove[i] = node_used(target, i) &&
                        descendant_of(target, i, target_home);
        for (u32 i = 1; i < target->node_count; i++)
            if (remove[i])
                memset(&target->nodes[i], 0, sizeof(target->nodes[i]));
    }

    u32 map[FYXFS_MAX_NODES];
    for (u32 i = 0; i < FYXFS_MAX_NODES; i++)
        map[i] = 0xFFFFFFFFu;
    for (u32 i = 1; i < current->node_count; i++) {
        if (!node_used(current, i) || !descendant_of(current, i, current_home))
            continue;
        u32 slot;
        int rc = alloc_node(target, &slot);
        if (rc != 0)
            return rc;
        target->nodes[slot] = current->nodes[i];
        map[i] = slot;
    }
    for (u32 old = 1; old < FYXFS_MAX_NODES; old++) {
        if (map[old] == 0xFFFFFFFFu)
            continue;
        u32 parent = current->nodes[old].parent;
        if (parent < FYXFS_MAX_NODES && map[parent] != 0xFFFFFFFFu)
            target->nodes[map[old]].parent = map[parent];
    }
    return FYX_OK;
}

int fyxfs_snapshot_rollback(u64 id, bool preserve_home)
{
    if (!g_fs.mounted)
        return -FYX_EINVAL;
    const struct fyxfs_snapshot_disk *snapshot = NULL;
    for (u32 i = 0; i < g_fs.root.root.snapshot_count; i++)
        if (g_fs.root.root.snapshots[i].id == id) {
            snapshot = &g_fs.root.root.snapshots[i];
            break;
        }
    if (!snapshot)
        return -FYX_ENOENT;
    u32 snapshot_root = snapshot->root_block;

    union fyxfs_root_block *old = kzalloc(sizeof(*old));
    if (!old)
        return -FYX_ENOMEM;
    int rc = read_root_at(g_fs.partition_lba, snapshot_root,
                          g_fs.total_blocks, old);
    if (rc != 0) {
        kfree(old);
        return rc;
    }
    struct fyxfs_root_disk next = old->root;
    kfree(old);

    /* 历史表不属于被恢复的系统状态，始终沿用当前版本，避免回滚后失忆。 */
    next.snapshot_count = g_fs.root.root.snapshot_count;
    memcpy(next.snapshots, g_fs.root.root.snapshots,
           sizeof(next.snapshots));
    if (preserve_home) {
        rc = merge_current_home(&next);
        if (rc != 0)
            return rc;
    }
    rc = commit_root(&next);
    if (rc == 0)
        kprintf("[snapshot] 回滚到 #%llu（%s /home），新 generation=%llu\n",
                (unsigned long long)id, preserve_home ? "保留" : "恢复",
                (unsigned long long)g_fs.root.root.generation);
    return rc;
}

/* -------------------------------- 检查 ----------------------------------- */
int fyxfs_check(bool repair, u32 *errors)
{
    UNUSED(repair);                    /* v1 只报告，不擅自修复在线数据 */
    u32 count = 0;
    if (!g_fs.mounted)
        return -FYX_EINVAL;

    union fyxfs_root_block *disk_root = kzalloc(sizeof(*disk_root));
    if (!disk_root)
        return -FYX_ENOMEM;
    int rc = read_root_at(g_fs.partition_lba, g_fs.active_root,
                          g_fs.total_blocks, disk_root);
    if (rc != 0)
        count++;
    kfree(disk_root);

    const struct fyxfs_root_disk *root = &g_fs.root.root;
    if (!node_used(root, 0) || root->nodes[0].type != FYXFS_NODE_DIR)
        count++;
    for (u32 i = 1; i < root->node_count; i++) {
        if (!node_used(root, i))
            continue;
        const struct fyxfs_node_disk *n = &root->nodes[i];
        if (!node_used(root, n->parent) ||
            root->nodes[n->parent].type != FYXFS_NODE_DIR ||
            n->name[0] == '\0' || n->extent_count > FYXFS_EXTENTS) {
            count++;
            continue;
        }
        u32 parent = i;
        bool cycle = false;
        for (u32 depth = 0; depth < FYXFS_MAX_NODES; depth++) {
            if (parent == 0)
                break;
            if (!node_used(root, parent)) {
                cycle = true;
                break;
            }
            parent = root->nodes[parent].parent;
            if (parent == i || depth + 1 == FYXFS_MAX_NODES) {
                cycle = true;
                break;
            }
        }
        if (cycle) {
            count++;
            continue;
        }
        if (n->type != FYXFS_NODE_FILE)
            continue;
        u64 capacity = 0;
        for (u32 e = 0; e < n->extent_count; e++) {
            capacity += (u64)n->extents[e].blocks * FYXFS_BLOCK_SIZE;
            if (n->extents[e].start >= g_fs.total_blocks ||
                n->extents[e].blocks > g_fs.total_blocks -
                                       n->extents[e].start)
                count++;
        }
        if (n->size > capacity) {
            count++;
            continue;
        }

        u32 crc = 0xFFFFFFFFu;
        u64 pos = 0;
        u8 chunk[512];
        while (pos < n->size) {
            size_t want = MIN((u64)sizeof(chunk), n->size - pos);
            size_t got = 0;
            rc = fyxfs_read_node(i, pos, chunk, want, &got);
            if (rc != 0 || got != want) {
                count++;
                break;
            }
            crc = crc32_update(crc, chunk, got);
            pos += got;
        }
        if (pos == n->size && ~crc != n->crc32)
            count++;
    }

    for (u32 i = 0; i < root->snapshot_count; i++) {
        union fyxfs_root_block *snap = kzalloc(sizeof(*snap));
        if (!snap)
            return -FYX_ENOMEM;
        rc = read_root_at(g_fs.partition_lba, root->snapshots[i].root_block,
                          g_fs.total_blocks, snap);
        kfree(snap);
        if (rc != 0)
            count++;
    }
    if (errors)
        *errors = count;
    kprintf("[fsck] %s：节点=%u 快照=%u 错误=%u\n",
            count ? "发现问题" : "校验通过", root->node_count,
            root->snapshot_count, count);
    return count ? -FYX_EIO : FYX_OK;
}

void fyxfs_get_stats(struct fyxfs_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!g_fs.mounted)
        return;
    out->partition_lba = g_fs.partition_lba;
    out->partition_sectors = g_fs.partition_sectors;
    out->generation = g_fs.root.root.generation;
    out->total_blocks = g_fs.total_blocks;
    out->next_block = g_fs.next_block;
    out->active_root = g_fs.active_root;
    out->nodes = 0;
    for (u32 i = 0; i < g_fs.root.root.node_count; i++)
        if (node_used(&g_fs.root.root, i))
            out->nodes++;
    out->snapshots = g_fs.root.root.snapshot_count;
}
