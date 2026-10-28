/* FYXOS — BIOS VBE 线性帧缓冲交接 */
#ifndef FYXOS_VBE_H
#define FYXOS_VBE_H

#include <fyxos/types.h>

struct vbe_info {
    volatile u32 *address;
    phys_addr_t physical;
    u32 width;
    u32 height;
    u32 pitch;
    u8 bpp;
    size_t size;
};

int  vbe_init(struct vbe_info *out);
bool vbe_present(void);

#endif /* FYXOS_VBE_H */
