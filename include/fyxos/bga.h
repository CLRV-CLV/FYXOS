/* FYXOS — Bochs Graphics Array 线性帧缓冲驱动 */
#ifndef FYXOS_BGA_H
#define FYXOS_BGA_H

#include <fyxos/types.h>

#define BGA_FRAMEBUFFER_PHYS 0xE0000000ull
#define BGA_WIDTH            800u
#define BGA_HEIGHT           600u
#define BGA_BPP              32u

struct bga_info {
    volatile u32 *address;
    phys_addr_t physical;
    u32 width;
    u32 height;
    u32 pitch;                  /* 每行字节数 */
    u8 bpp;
    size_t size;
};

int  bga_init(struct bga_info *out);
bool bga_present(void);
void bga_get_info(struct bga_info *out);

#endif /* FYXOS_BGA_H */
