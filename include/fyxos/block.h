/*
 * FYXOS — 块设备层
 *
 * 当前后端是自研 ATA PIO 驱动；对上层只暴露固定 512 字节扇区读写与
 * flush，不让文件系统依赖具体总线或端口。后续 AHCI/NVMe/VirtIO 可实现
 * 同一接口。
 */
#ifndef FYXOS_BLOCK_H
#define FYXOS_BLOCK_H

#include <fyxos/types.h>

#define BLOCK_SECTOR_SIZE 512u

struct block_stats {
    u64 sector_count;
    u64 read_sectors;
    u64 write_sectors;
    u64 flushes;
    u64 io_errors;
};

int  block_init(void);
int  block_read(u64 lba, void *buf, size_t count);
int  block_write(u64 lba, const void *buf, size_t count);
int  block_flush(void);
bool block_present(void);
u64  block_sector_count(void);
void block_get_stats(struct block_stats *out);

#endif /* FYXOS_BLOCK_H */
