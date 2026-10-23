/*
 * FYXOS — ATA PIO 块设备驱动
 *
 * 使用标准 ATA 寄存器与 LBA28 READ/WRITE SECTORS 命令，因此同一份代码既
 * 能在 fyxemu 的 ATA 模型上运行，也能在提供兼容 ATA 控制器的真机上运行。
 * 当前每次传输一个扇区；DMA、AHCI 与 NVMe 是后续性能工作，不改变块层 ABI。
 */
#include <fyxos/block.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
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

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7

#define ATA_WAIT_LIMIT 1000000u

static struct {
    bool present;
    u64 sectors;
    u16 identify[256];
    struct block_stats stats;
} g_ata;

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

static int ata_identify(void)
{
    outb(ATA_DRIVE, 0xA0);             /* 主通道主盘，LBA 高半字节为 0 */
    io_wait();
    outb(ATA_NSECTOR, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
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

int block_init(void)
{
    memset(&g_ata, 0, sizeof(g_ata));
    int rc = ata_identify();
    if (rc != 0) {
        g_ata.stats.sector_count = 0;
        return rc;
    }
    g_ata.stats.sector_count = g_ata.sectors;
    kprintf("[block] ATA PIO 就绪：%llu 扇区（%llu MiB）\n",
            (unsigned long long)g_ata.sectors,
            (unsigned long long)(g_ata.sectors / 2048));
    return FYX_OK;
}

bool block_present(void)
{
    return g_ata.present;
}

u64 block_sector_count(void)
{
    return g_ata.present ? g_ata.sectors : 0;
}

static int ata_transfer(u64 lba, void *buffer, bool write)
{
    if (!g_ata.present || lba >= g_ata.sectors || lba > 0x0FFFFFFFull)
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
        int rc = ata_transfer(lba + i, p + i * BLOCK_SECTOR_SIZE, false);
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
        int rc = ata_transfer(lba + i, (void *)(p + i * BLOCK_SECTOR_SIZE), true);
        if (rc != 0)
            return rc;
    }
    return FYX_OK;
}

int block_flush(void)
{
    if (!g_ata.present)
        return -FYX_ENOSYS;
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
