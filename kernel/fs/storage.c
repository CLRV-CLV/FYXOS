/*
 * FYXOS — 存储栈启动、系统分区识别与初始文件安装
 */
#include <fyxos/storage.h>
#include <fyxos/block.h>
#include <fyxos/vfs.h>
#include <fyxos/process.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>

static u64 g_partition_lba;
static u64 g_partition_sectors;

static u32 read_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static int find_system_partition(void)
{
    u8 mbr[BLOCK_SECTOR_SIZE];
    int rc = block_read(0, mbr, 1);
    if (rc != 0)
        return rc;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return -FYX_ENOENT;

    u64 sectors = block_sector_count();
    for (u32 i = 0; i < 4; i++) {
        const u8 *entry = mbr + 0x1BE + i * 16;
        u32 lba = read_le32(entry + 8);
        u32 count = read_le32(entry + 12);
        if (entry[4] == 0 || lba == 0 || count == 0)
            continue;
        if (entry[4] != 0x83)       /* 当前镜像的 FYXOS 数据分区类型 */
            continue;
        if ((u64)lba + count > sectors)
            return -FYX_EIO;
        g_partition_lba = lba;
        g_partition_sectors = count;
        return FYX_OK;
    }
    return -FYX_ENOENT;
}

static int install_initial_files(void)
{
    static const char *const dirs[] = {
        "/bin", "/etc", "/home", "/home/user", "/snapshots",
        "/tmp", "/var", "/usr", "/opt"
    };
    for (u32 i = 0; i < ARRAY_SIZE(dirs); i++) {
        u32 node;
        if (fyxfs_lookup(dirs[i], &node) == FYX_OK)
            continue;
        int rc = vfs_mkdir(dirs[i]);
        if (rc != 0) {
            kprintf("[storage] 创建 %s 失败：%d\n", dirs[i], rc);
            return rc;
        }
    }

    struct vfs_stat st;
    if (vfs_stat("/bin/init", &st) != FYX_OK) {
        size_t size = (size_t)(g_init_image_end - g_init_image_start);
        if (!size)
            return -FYX_EINVAL;
        int rc = vfs_write_file("/bin/init", g_init_image_start, size);
        if (rc != 0) {
            kprintf("[storage] 安装 /bin/init 失败：%d\n", rc);
            return rc;
        }
        kprintf("[storage] 已把 init 安装到持久化 /bin/init（%u 字节）\n",
                (unsigned)size);
    }

    const char motd[] = "FYXOS：数据与快照均由 FYXFS CoW 事务保护。\n";
    if (vfs_stat("/etc/motd", &st) != FYX_OK) {
        int rc = vfs_write_file("/etc/motd", motd, sizeof(motd) - 1);
        if (rc != 0)
            return rc;
    }
    return FYX_OK;
}

int storage_mount(void)
{
    if (!g_partition_lba)
        return -FYX_EINVAL;
    return vfs_mount(g_partition_lba, g_partition_sectors);
}

int storage_unmount(void)
{
    return vfs_unmount();
}

static int super_slots_are_blank(void)
{
    u8 sector[BLOCK_SECTOR_SIZE];
    for (u32 slot = 0; slot < 2; slot++) {
        int rc = block_read(g_partition_lba + slot, sector, 1);
        if (rc != 0)
            return rc;
        for (u32 i = 0; i < sizeof(sector); i++)
            if (sector[i] != 0)
                return -FYX_EBUSY;  /* 有数据但不可挂载：不能擅自格式化 */
    }
    return FYX_OK;
}

int storage_init(void)
{
    int rc = block_init();
    if (rc != 0) {
        kprintf("[storage] ATA 初始化失败：%d\n", rc);
        return rc;
    }
    rc = find_system_partition();
    if (rc != 0) {
        kprintf("[storage] 未找到 FYXOS 系统分区：%d\n", rc);
        return rc;
    }
    kprintf("[storage] 系统分区 LBA=%llu，扇区数=%llu\n",
            (unsigned long long)g_partition_lba,
            (unsigned long long)g_partition_sectors);

    rc = storage_mount();
    if (rc == -FYX_ENOENT) {
        rc = super_slots_are_blank();
        if (rc == 0) {
            kprintf("[storage] 超级块槽全零，初始化新的 FYXFS 卷\n");
            rc = vfs_format(g_partition_lba, g_partition_sectors);
            if (rc == 0)
                rc = storage_mount();
        } else {
            kprintf("[storage] 分区含非 FYXFS 数据，拒绝自动格式化\n");
        }
    }
    if (rc != 0) {
        kprintf("[storage] FYXFS 挂载失败：%d（不会静默格式化损坏卷）\n", rc);
        return rc;
    }

    rc = install_initial_files();
    if (rc != 0)
        return rc;
    u32 errors = 0;
    rc = vfs_check(false, &errors);
    if (rc != 0)
        return rc;

    struct fyxfs_stats stats;
    vfs_stats(&stats);
    kprintf("[storage] 持久化文件系统就绪：%u 节点，generation=%llu，"
            "已分配 %u/%u 块\n", stats.nodes,
            (unsigned long long)stats.generation,
            stats.next_block, stats.total_blocks);
    return FYX_OK;
}

u64 storage_partition_lba(void)
{
    return g_partition_lba;
}

u64 storage_partition_sectors(void)
{
    return g_partition_sectors;
}
