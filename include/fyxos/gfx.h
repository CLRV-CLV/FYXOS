/* FYXOS — 自研软件光栅化与双缓冲呈现 */
#ifndef FYXOS_GFX_H
#define FYXOS_GFX_H

#include <fyxos/types.h>

#define GFX_RGB(r,g,b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

struct gfx_stats {
    u64 frames;
    u64 draw_calls;
    u64 pixels_written;
    u64 blit_bytes;
    u32 last_checksum;
    u32 width;
    u32 height;
    phys_addr_t backbuffer_phys;
};

int  gfx_init(void);
bool gfx_available(void);
void gfx_clear(u32 color);
void gfx_pixel(i32 x, i32 y, u32 color);
void gfx_fill_rect(i32 x, i32 y, i32 w, i32 h, u32 color);
void gfx_draw_rect(i32 x, i32 y, i32 w, i32 h, u32 color);
void gfx_fill_round_rect(i32 x, i32 y, i32 w, i32 h, i32 radius, u32 color);
void gfx_fill_circle(i32 cx, i32 cy, i32 radius, u32 color);
void gfx_draw_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 color);
void gfx_draw_text(i32 x, i32 y, const char *text, u32 color, u32 scale);
u32  gfx_text_width(const char *text, u32 scale);
u32  gfx_present(void);             /* 背 buffer → LFB，返回 FNV-1a 校验 */
u32  gfx_checksum(void);            /* 当前 LFB 可见字节的 FNV-1a */
void gfx_get_stats(struct gfx_stats *out);

#endif /* FYXOS_GFX_H */
