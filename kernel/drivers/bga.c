/* FYXOS — BGA 模式设置与 LFB 映射 */
#include <fyxos/bga.h>
#include <fyxos/mm.h>
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>

#define BGA_INDEX_PORT       0x1CE
#define BGA_DATA_PORT        0x1CF
#define BGA_REG_ID           0x0
#define BGA_REG_XRES         0x1
#define BGA_REG_YRES         0x2
#define BGA_REG_BPP          0x3
#define BGA_REG_ENABLE       0x4
#define BGA_REG_VIRT_WIDTH   0x6
#define BGA_REG_VIRT_HEIGHT  0x7
#define BGA_REG_X_OFFSET     0x8
#define BGA_REG_Y_OFFSET     0x9
#define BGA_REG_MEMORY_64K   0xA
#define BGA_ENABLE           0x0001
#define BGA_ENABLE_LFB       0x0040

static struct bga_info g_bga;
static bool g_present;

static void bga_write(u16 index, u16 value)
{
    outw(BGA_INDEX_PORT, index);
    outw(BGA_DATA_PORT, value);
}

static u16 bga_read(u16 index)
{
    outw(BGA_INDEX_PORT, index);
    return inw(BGA_DATA_PORT);
}

bool bga_present(void) { return g_present; }

void bga_get_info(struct bga_info *out)
{
    if (out)
        *out = g_bga;
}

int bga_init(struct bga_info *out)
{
    if (g_present) {
        bga_get_info(out);
        return FYX_OK;
    }
    memset(&g_bga, 0, sizeof(g_bga));
    bga_write(BGA_REG_ID, 0xB0C5);
    u16 id = bga_read(BGA_REG_ID);
    if (id < 0xB0C0 || id > 0xB0C5)
        return -FYX_ENOSYS;

    bga_write(BGA_REG_ENABLE, 0);
    bga_write(BGA_REG_XRES, BGA_WIDTH);
    bga_write(BGA_REG_YRES, BGA_HEIGHT);
    bga_write(BGA_REG_BPP, BGA_BPP);
    bga_write(BGA_REG_VIRT_WIDTH, BGA_WIDTH);
    bga_write(BGA_REG_VIRT_HEIGHT, BGA_HEIGHT);
    bga_write(BGA_REG_X_OFFSET, 0);
    bga_write(BGA_REG_Y_OFFSET, 0);
    bga_write(BGA_REG_ENABLE, BGA_ENABLE | BGA_ENABLE_LFB);

    size_t size = BGA_WIDTH * BGA_HEIGHT * (BGA_BPP / 8);
    if (bga_read(BGA_REG_XRES) != BGA_WIDTH ||
        bga_read(BGA_REG_YRES) != BGA_HEIGHT ||
        bga_read(BGA_REG_BPP) != BGA_BPP ||
        (size_t)bga_read(BGA_REG_MEMORY_64K) * 65536u < size) {
        bga_write(BGA_REG_ENABLE, 0);
        return -FYX_EIO;
    }

    u64 virt = (u64)(uintptr_t)phys_to_virt(BGA_FRAMEBUFFER_PHYS);
    if (vmm_map(virt, BGA_FRAMEBUFFER_PHYS, ALIGN_UP(size, PAGE_SIZE),
                VMM_WRITE | VMM_NOEXEC | VMM_NOCACHE) != 0) {
        bga_write(BGA_REG_ENABLE, 0);
        return -FYX_ENOMEM;
    }

    g_bga.address = (volatile u32 *)(uintptr_t)virt;
    g_bga.physical = BGA_FRAMEBUFFER_PHYS;
    g_bga.width = BGA_WIDTH;
    g_bga.height = BGA_HEIGHT;
    g_bga.pitch = BGA_WIDTH * 4;
    g_bga.bpp = BGA_BPP;
    g_bga.size = size;
    g_present = true;

    volatile u64 *clear = (volatile u64 *)g_bga.address;
    for (size_t i = 0; i < size / 8; i++)
        clear[i] = 0;
    bga_get_info(out);
    kprintf("[gfx] BGA LFB %ux%ux%u phys=0x%llx pitch=%u\n",
            BGA_WIDTH, BGA_HEIGHT, BGA_BPP,
            (unsigned long long)BGA_FRAMEBUFFER_PHYS, g_bga.pitch);
    return FYX_OK;
}
