/*
 * FYXOS — 引导协议 (boot protocol)
 * ---------------------------------------------------------------------------
 * 本文件是 stage2（实模式/32 位 C）与内核之间**唯一**的契约。
 * 结构体按 1 字节对齐、字段偏移固定，汇编侧按数值偏移读写，
 * 因此新增字段只能追加，禁止在中间插入或改变字段宽度。
 *
 * 物理内存布局（引导期，进入长模式之前）
 *   0x00000500 - 0x00000FFF  stage1 的 DAP 暂存区
 *   0x00005000 - 0x00006FFF  boot_info（本结构 + E820 表）
 *   0x00007000 - 0x00007BFF  实模式/32 位临时栈（向下生长）
 *   0x00007C00 - 0x00007DFF  stage1 (MBR)
 *   0x00008000 - 0x0000FFEF  stage2（末尾 16 字节为描述符）
 *   0x00010000 - 0x0001FFFF  ELF 内核映像暂存区（由 stage1/2 用 BIOS 读入）
 *   0x00020000 - 0x0002FFFF  引导期页表（PML4/PDPT/PD，由 stage2_32 构造）
 *   0x00100000 - 0x0017FFFF  内核 ELF 文件暂存（最多 512 KiB）
 *   0x00200000 -             内核 PT_LOAD 目标物理地址（2 MiB 对齐，便于大页映射）
 */
#ifndef FYXOS_BOOT_INFO_H
#define FYXOS_BOOT_INFO_H

#include <fyxos/types.h>

#define FYX_BOOT_MAGIC   0x31585946u   /* "FYX1" little-endian */
#define FYX_BOOT_VERSION 1u

#define FYX_MEM_USABLE   1u
#define FYX_MEM_RESERVED 2u
#define FYX_MEM_ACPI     3u
#define FYX_MEM_NVS      4u
#define FYX_MEM_BADRAM   5u

#define FYX_E820_MAX     64u

/* E820 表项：与 BIOS INT 15h EAX=E820h 返回格式一致（24 字节） */
struct fyx_e820_entry {
    u64  base;
    u64  length;
    u32  type;
    u32  acpi_attrs;
} __attribute__((packed));

/*
 * 固定偏移布局（见上方物理内存图，基址 0x5000）：
 *   0x00 magic            0x28 hhdm_base          0x4A fb_bpp
 *   0x04 version          0x30 total_memory       0x50 e820[]
 *   0x08 e820_count       0x38 framebuffer_addr
 *   0x0C boot_drive       0x40 framebuffer_pitch
 *   0x10 kernel_phys      0x44 framebuffer_width
 *   0x18 kernel_size      0x48 framebuffer_height
 *   0x20 kernel_virt
 */
struct fyx_boot_info {
    u32 magic;
    u32 version;

    u32 e820_count;
    u32 boot_drive;

    u64 kernel_phys;        /* 内核映像起始物理地址 */
    u64 kernel_size;        /* 内核映像占用物理字节数 */
    u64 kernel_virt;        /* 内核链接的虚拟基址（0xFFFFFFFF80000000） */
    u64 hhdm_base;          /* 物理内存直接映射窗口基址 */

    u64 total_memory;       /* E820 统计出的可用物理内存总量 */

    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u16 framebuffer_bpp;
    u16 _pad;

    struct fyx_e820_entry e820[FYX_E820_MAX];
} __attribute__((packed));

/* 汇编侧使用的数值偏移；改动结构体时必须同步修改 boot 目录下的 .S 文件 */
#define BI_MAGIC            0x00
#define BI_VERSION          0x04
#define BI_E820_COUNT       0x08
#define BI_BOOT_DRIVE       0x0C
#define BI_KERNEL_PHYS      0x10
#define BI_KERNEL_SIZE      0x18
#define BI_KERNEL_VIRT      0x20
#define BI_HHDM_BASE        0x28
#define BI_TOTAL_MEMORY     0x30
#define BI_E820_TABLE       0x50

#define BOOT_INFO_ADDR      0x5000u

/* 内存布局常量（汇编与 C 共用） */
#define KERNEL_PHYS_BASE    0x200000ull
#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ull
#define HHDM_BASE           0xFFFF800000000000ull

/*
 * ELF 文件暂存区：必须是**实模式 segment:offset 能直接寻址**的地址。
 *
 * 这里曾经写成 0x100000，但 stage2_16.S 里的 KERNEL_BUF_SEG=0x1000 在实模式下
 * 只能表示 0x1000*16 = 0x10000 —— BIOS INT 13h AH=42h 的缓冲区是 seg:off 形式，
 * 上限只有 0x10FFEF，一次传输根本到不了 1 MiB。结果内核被读进了 0x10000，
 * 而 stage2_32 去 0x100000 找 ELF，只看到一片 0，报 "not an ELF"。
 *
 * 现在的方案：暂存到低端 0x10000，进入 32 位平坦模式后再由 stage2_32 把
 * PT_LOAD 段复制到最终的 0x200000（KERNEL_PHYS_BASE）。
 *
 * 布局注意：ELF 暂存区随内核增长，可能经过 0x50000 页表池；页表只在所有
 * PT_LOAD 已复制完后才建立，因此该复用是安全的。32 位 C 栈则在段复制期间
 * 活跃，已迁到高端 0x1F0000-0x1FEFFF（栈顶 0x1FF000），不再覆盖暂存 ELF。
 *
 *   0x10000起         内核 ELF 暂存（实际大小由镜像描述符给出）
 *   0x50000-0x5FFFF  引导期页表池（段复制完成后使用）
 *   0x1F0000起       32 位栈（高端可用 RAM，向下生长）
 */
#define KERNEL_STAGE_ADDR   0x10000u    /* ELF 文件暂存（实模式可达） */
#define BOOT_PAGETABLE_ADDR 0x50000u    /* 引导期页表池 */

#define STAGE2_ADDR         0x8000u
#define STAGE2_MAX_SIZE     32752u      /* 末尾 16 字节留给描述符 */
#define STAGE2_DESC_OFF     (STAGE2_ADDR + STAGE2_MAX_SIZE)
#define STAGE2_DESC_MAGIC   0x32585946u /* "FYX2" */

#endif /* FYXOS_BOOT_INFO_H */
