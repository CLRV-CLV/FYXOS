/*
 * FYXOS — 使用 stage2 通过 BIOS VBE 取得的 800x600x32 线性帧缓冲。
 * 进入长模式后不再调用 BIOS，只把物理 LFB 以 NOCACHE 映射进内核 HHDM。
 */
#include <fyxos/vbe.h>
#include <fyxos/boot_info.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>

static struct vbe_info g_vbe;
static bool g_present;

bool vbe_present(void)
{
    return g_present;
}

int vbe_init(struct vbe_info *out)
{
    if (g_present) {
        if (out)
            *out = g_vbe;
        return FYX_OK;
    }
    struct fyx_boot_info *bi = kernel_boot_info();
    if (!bi || !bi->framebuffer_addr)
        return -FYX_ENOSYS;

    u32 width = bi->framebuffer_width;
    u32 height = bi->framebuffer_height;
    u32 pitch = bi->framebuffer_pitch;
    if (width != 800 || height != 600 || bi->framebuffer_bpp != 32 ||
        pitch < width * 4 || (pitch & 7u) != 0)
        return -FYX_ENOSYS;
    u64 size64 = (u64)pitch * height;
    if (!size64 || size64 > (64ull << 20))
        return -FYX_EIO;

    phys_addr_t physical = bi->framebuffer_addr;
    u64 virt = (u64)(uintptr_t)phys_to_virt(physical);
    size_t size = (size_t)size64;
    if (vmm_map(virt, physical, ALIGN_UP(size, PAGE_SIZE),
                VMM_WRITE | VMM_NOEXEC | VMM_NOCACHE) != 0)
        return -FYX_ENOMEM;

    memset(&g_vbe, 0, sizeof(g_vbe));
    g_vbe.address = (volatile u32 *)(uintptr_t)virt;
    g_vbe.physical = physical;
    g_vbe.width = width;
    g_vbe.height = height;
    g_vbe.pitch = pitch;
    g_vbe.bpp = 32;
    g_vbe.size = size;
    g_present = true;

    /* 清除 BIOS 文本/旧画面；桌面随后用软件背 buffer 全量重绘。 */
    volatile u64 *clear = (volatile u64 *)g_vbe.address;
    for (size_t i = 0; i < size / 8; i++)
        clear[i] = 0;
    if (out)
        *out = g_vbe;
    kprintf("[gfx] VBE LFB %ux%ux%u phys=0x%llx pitch=%u\n",
            width, height, g_vbe.bpp, (unsigned long long)physical, pitch);
    return FYX_OK;
}
