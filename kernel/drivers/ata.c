/*
 * FYXOS — ATA PIO / ATAPI Live CD 块设备驱动
 *
 * 持久磁盘使用标准 ATA LBA28 READ/WRITE SECTORS。Live ISO 启动后，BIOS 的
 * El Torito 仿真已经消失，因此另外实现 IDENTIFY PACKET、PACKET、SCSI
 * READ CAPACITY(10) 与 READ(10)，直接从光盘读取内嵌 FYXOS.IMG。
 *
 * 光盘本身只读；块层用 512 字节 RAM overlay 承接 FYXFS 写入。这样同一份
 * VFS/CoW/安装器无需特殊分支即可在 Live 会话运行，但重启后修改自然消失。
 */
#include <fyxos/block.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/sync.h>
#include <fyxos/x86_64.h>

#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_FEATURES   0x1F1
#define ATA_NSECTOR    0x1F2
#define ATA_LBA_LO     0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HI     0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALTSTATUS  0x3F6

#define ATA_SR_ERR     0x01
#define ATA_SR_DRQ     0x08
#define ATA_SR_DF      0x20
#define ATA_SR_DRDY    0x40
#define ATA_SR_BSY     0x80

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_READ            0x20
#define ATA_CMD_WRITE           0x30
#define ATA_CMD_FLUSH           0xE7
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0

#define ATAPI_SECTOR_SIZE 2048u
#define ATAPI_DESCR_LBA  1u
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define LIVE_MAGIC       "FYXOSCD1"
#define LIVE_VERSION     1u
#define LIVE_DESC_SIZE   32u
#define OVERLAY_BUCKETS  64u

#define ATA_WAIT_LIMIT 1000000u

struct live_overlay {
    struct live_overlay *next;
    u64 lba;
    u8 data[BLOCK_SECTOR_SIZE];
};

static struct {
    bool present;
    bool live_cd;
    u64 sectors;                    /* 对上层可见的 512 字节扇区数 */
    u64 cd_sectors;                 /* 物理光盘的 2048 字节扇区数 */
    u32 cd_image_lba;
    u32 cd_cache_lba;
    bool cd_cache_valid;
    u8 cd_cache[ATAPI_SECTOR_SIZE];
    struct live_overlay *overlay[OVERLAY_BUCKETS];
    u16 identify[256];
    struct block_stats stats;
} g_ata;

static u32 get_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static u64 get_le64(const u8 *p)
{
    return (u64)get_le32(p) | ((u64)get_le32(p + 4) << 32);
}

static u32 get_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

static void put_be16(u8 *p, u16 value)
{
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void put_be32(u8 *p, u32 value)
{
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static u32 live_crc32(const void *data, size_t size)
{
    const u8 *p = data;
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (u32 bit = 0; bit < 8; bit++) {
            bool low = (crc & 1u) != 0;
            crc >>= 1;
            if (low)
                crc ^= 0xEDB88320u;
        }
    }
    return ~crc;
}

static int ata_wait_not_busy(void)
{
    for (u32 i = 0; i < ATA_WAIT_LIMIT; i++) {
        u8 status = inb(ATA_ALTSTATUS);
        if (!(status & ATA_SR_BSY))
            return FYX_OK;
        io_wait();
    }
    return -FYX_EIO;
}

static int ata_wait_data(void)
{
    for (u32 i = 0; i < ATA_WAIT_LIMIT; i++) {
        u8 status = inb(ATA_ALTSTATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return -FYX_EIO;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
            return FYX_OK;
        io_wait();
    }
    return -FYX_EIO;
}

static int ata_check_result(void)
{
    u8 status = inb(ATA_STATUS);
    if (status & (ATA_SR_ERR | ATA_SR_DF))
        return -FYX_EIO;
    return FYX_OK;
}

static void ata_select(void)
{
    outb(ATA_DRIVE, 0xA0);             /* 主通道主盘/主光驱 */
    io_wait();
    outb(ATA_NSECTOR, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
}

static int ata_identify(void)
{
    ata_select();
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    u8 status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFF)
        return -FYX_ENOSYS;
    if (ata_wait_data() != 0)
        return -FYX_EIO;

    for (u32 i = 0; i < 256; i++)
        g_ata.identify[i] = inw(ATA_DATA);
    if (ata_check_result() != 0)
        return -FYX_EIO;

    /* IDENTIFY word 60/61：LBA28 可寻址扇区数。 */
    u64 sectors = (u64)g_ata.identify[60] |
                  ((u64)g_ata.identify[61] << 16);
    if (sectors == 0)
        return -FYX_EIO;
    g_ata.sectors = sectors;
    g_ata.present = true;
    return FYX_OK;
}

static int atapi_identify(void)
{
    ata_select();
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    u8 status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFF)
        return -FYX_ENOSYS;
    if (ata_wait_data() != 0)
        return -FYX_EIO;
    for (u32 i = 0; i < 256; i++)
        g_ata.identify[i] = inw(ATA_DATA);
    return ata_check_result();
}

/* 发送 12 字节 SCSI CDB 并读回一段数据。调用者可处于关中断状态。 */
static int atapi_packet(const u8 cdb[12], void *buffer, size_t size)
{
    if (!cdb || (size && !buffer) || size > 0xFFFFu)
        return -FYX_EINVAL;

    u64 irq = irq_save_disable();
    int rc = ata_wait_not_busy();
    if (rc != 0)
        goto out;
    outb(ATA_DRIVE, 0xA0);
    outb(ATA_FEATURES, 0);
    outb(ATA_NSECTOR, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, (u8)size);
    outb(ATA_LBA_HI, (u8)(size >> 8));
    outb(ATA_COMMAND, ATA_CMD_PACKET);
    rc = ata_wait_data();
    if (rc != 0)
        goto out;

    u16 packet[6];
    memcpy(packet, cdb, sizeof(packet));
    for (u32 i = 0; i < 6; i++)
        outw(ATA_DATA, packet[i]);
    if (size == 0) {
        rc = ata_wait_not_busy();
        if (rc == 0)
            rc = ata_check_result();
        goto out;
    }

    rc = ata_wait_data();
    u8 *out = buffer;
    for (size_t pos = 0; rc == 0 && pos < size; pos += 2) {
        u16 word = inw(ATA_DATA);
        out[pos] = (u8)word;
        if (pos + 1 < size)
            out[pos + 1] = (u8)(word >> 8);
    }
    if (rc == 0)
        rc = ata_wait_not_busy();
    if (rc == 0)
        rc = ata_check_result();
out:
    irq_restore(irq);
    return rc;
}

static int atapi_read_capacity(void)
{
    u8 cdb[12] = {0};
    u8 response[8];
    cdb[0] = SCSI_READ_CAPACITY_10;
    int rc = atapi_packet(cdb, response, sizeof(response));
    if (rc != 0)
        return rc;
    u32 last_lba = get_be32(response);
    u32 block_size = get_be32(response + 4);
    if (last_lba == 0xFFFFFFFFu || block_size != ATAPI_SECTOR_SIZE)
        return -FYX_EIO;
    g_ata.cd_sectors = (u64)last_lba + 1;
    return FYX_OK;
}

static int atapi_read_physical(u32 lba, void *buffer)
{
    if ((u64)lba >= g_ata.cd_sectors)
        return -FYX_EINVAL;
    u8 cdb[12] = {0};
    cdb[0] = SCSI_READ_10;
    put_be32(cdb + 2, lba);
    put_be16(cdb + 7, 1);
    return atapi_packet(cdb, buffer, ATAPI_SECTOR_SIZE);
}

static int atapi_cached_sector(u32 lba)
{
    if (g_ata.cd_cache_valid && g_ata.cd_cache_lba == lba)
        return FYX_OK;
    int rc = atapi_read_physical(lba, g_ata.cd_cache);
    if (rc != 0) {
        g_ata.cd_cache_valid = false;
        return rc;
    }
    g_ata.cd_cache_lba = lba;
    g_ata.cd_cache_valid = true;
    return FYX_OK;
}

static int atapi_live_init(void)
{
    int rc = atapi_read_capacity();
    if (rc != 0)
        return rc;
    if (g_ata.cd_sectors <= ATAPI_DESCR_LBA)
        return -FYX_ENOENT;
    rc = atapi_cached_sector(ATAPI_DESCR_LBA);
    if (rc != 0)
        return rc;

    const u8 *desc = g_ata.cd_cache;
    if (memcmp(desc, LIVE_MAGIC, 8) != 0 || get_le32(desc + 8) != LIVE_VERSION)
        return -FYX_ENOENT;
    if (get_le32(desc + 28) != live_crc32(desc, 28))
        return -FYX_EIO;
    u32 image_lba = get_le32(desc + 12);
    u64 image_size = get_le64(desc + 16);
    u32 image_sectors = get_le32(desc + 24);
    if (image_lba < 16 || image_size == 0 ||
        (image_size & (BLOCK_SECTOR_SIZE - 1)) != 0 ||
        image_sectors != image_size / BLOCK_SECTOR_SIZE)
        return -FYX_EIO;
    if ((u64)image_lba > g_ata.cd_sectors ||
        image_size > (g_ata.cd_sectors - image_lba) * ATAPI_SECTOR_SIZE)
        return -FYX_EIO;

    rc = atapi_cached_sector(image_lba);
    if (rc != 0)
        return rc;
    if (g_ata.cd_cache[510] != 0x55 || g_ata.cd_cache[511] != 0xAA ||
        memcmp(g_ata.cd_cache + 0x1B0, "FYXB", 4) != 0)
        return -FYX_EIO;

    g_ata.cd_image_lba = image_lba;
    g_ata.sectors = image_sectors;
    g_ata.live_cd = true;
    g_ata.present = true;
    return FYX_OK;
}

int block_init(void)
{
    memset(&g_ata, 0, sizeof(g_ata));
    int ata_rc = ata_identify();
    if (ata_rc == 0) {
        g_ata.stats.sector_count = g_ata.sectors;
        kprintf("[block] ATA PIO 就绪：%llu 扇区（%llu MiB）\n",
                (unsigned long long)g_ata.sectors,
                (unsigned long long)(g_ata.sectors / 2048));
        return FYX_OK;
    }

    int cd_rc = atapi_identify();
    if (cd_rc == 0)
        cd_rc = atapi_live_init();
    if (cd_rc == 0) {
        g_ata.stats.sector_count = g_ata.sectors;
        kprintf("[block] ATAPI Live CD 就绪：%llu 个 512 字节扇区，"
                "FYXOS.IMG @ CD LBA %u\n",
                (unsigned long long)g_ata.sectors, g_ata.cd_image_lba);
        kprintf("[block] Live 写入仅进入 RAM overlay，重启后丢失\n");
        return FYX_OK;
    }
    g_ata.stats.sector_count = 0;
    return ata_rc;
}

bool block_present(void)
{
    return g_ata.present;
}

bool block_is_volatile(void)
{
    return g_ata.present && g_ata.live_cd;
}

u64 block_sector_count(void)
{
    return g_ata.present ? g_ata.sectors : 0;
}

static int ata_transfer(u64 lba, void *buffer, bool write)
{
    if (!g_ata.present || g_ata.live_cd || lba >= g_ata.sectors ||
        lba > 0x0FFFFFFFull)
        return -FYX_EINVAL;

    u64 irq = irq_save_disable();
    int rc = ata_wait_not_busy();
    if (rc != 0)
        goto out;

    outb(ATA_DRIVE, 0xE0 | (u8)((lba >> 24) & 0x0F));
    outb(ATA_FEATURES, 0);
    outb(ATA_NSECTOR, 1);
    outb(ATA_LBA_LO, (u8)lba);
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI, (u8)(lba >> 16));
    outb(ATA_COMMAND, write ? ATA_CMD_WRITE : ATA_CMD_READ);

    rc = ata_wait_data();
    if (rc != 0)
        goto out;

    u16 words[256];
    if (write) {
        memcpy(words, buffer, sizeof(words));
        for (u32 i = 0; i < 256; i++)
            outw(ATA_DATA, words[i]);
    } else {
        for (u32 i = 0; i < 256; i++)
            words[i] = inw(ATA_DATA);
        memcpy(buffer, words, sizeof(words));
    }

    rc = ata_wait_not_busy();
    if (rc == 0)
        rc = ata_check_result();

out:
    irq_restore(irq);
    if (rc != 0)
        g_ata.stats.io_errors++;
    else if (write)
        g_ata.stats.write_sectors++;
    else
        g_ata.stats.read_sectors++;
    return rc;
}

static struct live_overlay *overlay_find(u64 lba)
{
    struct live_overlay *entry = g_ata.overlay[lba % OVERLAY_BUCKETS];
    while (entry) {
        if (entry->lba == lba)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static int live_read(u64 lba, void *buffer)
{
    u64 irq = irq_save_disable();
    struct live_overlay *entry = overlay_find(lba);
    if (entry) {
        memcpy(buffer, entry->data, BLOCK_SECTOR_SIZE);
        g_ata.stats.read_sectors++;
        irq_restore(irq);
        return FYX_OK;
    }

    u64 physical = (u64)g_ata.cd_image_lba + lba / 4;
    int rc = physical > 0xFFFFFFFFu ? -FYX_EINVAL :
             atapi_cached_sector((u32)physical);
    if (rc == 0) {
        memcpy(buffer, g_ata.cd_cache + (lba & 3u) * BLOCK_SECTOR_SIZE,
               BLOCK_SECTOR_SIZE);
        g_ata.stats.read_sectors++;
    } else {
        g_ata.stats.io_errors++;
    }
    irq_restore(irq);
    return rc;
}

static int live_write(u64 lba, const void *buffer)
{
    u64 irq = irq_save_disable();
    struct live_overlay *entry = overlay_find(lba);
    if (!entry) {
        entry = kzalloc(sizeof(*entry));
        if (!entry) {
            g_ata.stats.io_errors++;
            irq_restore(irq);
            return -FYX_ENOMEM;
        }
        entry->lba = lba;
        u32 bucket = lba % OVERLAY_BUCKETS;
        entry->next = g_ata.overlay[bucket];
        g_ata.overlay[bucket] = entry;
    }
    memcpy(entry->data, buffer, BLOCK_SECTOR_SIZE);
    g_ata.stats.write_sectors++;
    irq_restore(irq);
    return FYX_OK;
}

static bool range_valid(u64 lba, size_t count)
{
    return g_ata.present && count != 0 && lba < g_ata.sectors &&
           count <= g_ata.sectors - lba;
}

int block_read(u64 lba, void *buf, size_t count)
{
    if (!buf || !range_valid(lba, count))
        return -FYX_EINVAL;
    u8 *p = buf;
    for (size_t i = 0; i < count; i++) {
        int rc = g_ata.live_cd ? live_read(lba + i, p + i * BLOCK_SECTOR_SIZE) :
                 ata_transfer(lba + i, p + i * BLOCK_SECTOR_SIZE, false);
        if (rc != 0)
            return rc;
    }
    return FYX_OK;
}

int block_write(u64 lba, const void *buf, size_t count)
{
    if (!buf || !range_valid(lba, count))
        return -FYX_EINVAL;
    const u8 *p = buf;
    for (size_t i = 0; i < count; i++) {
        int rc = g_ata.live_cd ? live_write(lba + i, p + i * BLOCK_SECTOR_SIZE) :
                 ata_transfer(lba + i, (void *)(p + i * BLOCK_SECTOR_SIZE), true);
        if (rc != 0)
            return rc;
    }
    return FYX_OK;
}

int block_flush(void)
{
    if (!g_ata.present)
        return -FYX_ENOSYS;
    if (g_ata.live_cd) {
        /* overlay 已在 RAM 中立即可见；这里保持 FYXFS 的事务调用语义。 */
        g_ata.stats.flushes++;
        return FYX_OK;
    }
    u64 irq = irq_save_disable();
    int rc = ata_wait_not_busy();
    if (rc == 0) {
        outb(ATA_DRIVE, 0xE0);
        outb(ATA_COMMAND, ATA_CMD_FLUSH);
        rc = ata_wait_not_busy();
    }
    if (rc == 0)
        rc = ata_check_result();
    irq_restore(irq);
    if (rc != 0)
        g_ata.stats.io_errors++;
    else
        g_ata.stats.flushes++;
    return rc;
}

void block_get_stats(struct block_stats *out)
{
    if (out)
        *out = g_ata.stats;
}
