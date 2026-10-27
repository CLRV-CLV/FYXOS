/* FYXOS — 启动存储栈与系统分区 */
#ifndef FYXOS_STORAGE_H
#define FYXOS_STORAGE_H

#include <fyxos/types.h>

int storage_init(void);
int storage_mount(void);
int storage_unmount(void);
u64 storage_partition_lba(void);
u64 storage_partition_sectors(void);

#endif /* FYXOS_STORAGE_H */
