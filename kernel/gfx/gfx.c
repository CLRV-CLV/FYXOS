/* FYXOS — 软件像素、矩形、圆角、线条、5x7 字体与双缓冲 blit */
#include <fyxos/gfx.h>
#include <fyxos/bga.h>
#include <fyxos/mm.h>
#include <fyxos/lib.h>

static volatile u32 *g_front;
static u32 *g_back;
static u32 g_width, g_height, g_pitch;
static size_t g_size;
static phys_addr_t g_back_phys;
static bool g_ready;
static struct gfx_stats g_stats;

struct font_glyph {
    char ch;
    u8 row[7];
};

/* 5x7，bit4 是最左像素；小写在绘制时折叠成大写。 */
static const struct font_glyph g_font[] = {
    { ' ', { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 } },
    { '?', { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 } },
    { '0', { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E } },
    { '1', { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E } },
    { '2', { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F } },
    { '3', { 0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E } },
    { '4', { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 } },
    { '5', { 0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E } },
    { '6', { 0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E } },
    { '7', { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 } },
    { '8', { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E } },
    { '9', { 0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E } },
    { 'A', { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'B', { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E } },
    { 'C', { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E } },
    { 'D', { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E } },
    { 'E', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F } },
    { 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } },
    { 'G', { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0F } },
    { 'H', { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'I', { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E } },
    { 'J', { 0x01,0x01,0x01,0x01,0x11,0x11,0x0E } },
    { 'K', { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 } },
    { 'L', { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F } },
    { 'M', { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 } },
    { 'N', { 0x11,0x19,0x19,0x15,0x13,0x13,0x11 } },
    { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'P', { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 } },
    { 'Q', { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D } },
    { 'R', { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 } },
    { 'S', { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E } },
    { 'T', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 } },
    { 'U', { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'V', { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 } },
    { 'W', { 0x11,0x11,0x11,0x15,0x15,0x15,0x0A } },
    { 'X', { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 } },
    { 'Y', { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 } },
    { 'Z', { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F } },
    { ':', { 0x00,0x04,0x04,0x00,0x04,0x04,0x00 } },
    { '.', { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C } },
    { '-', { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 } },
    { '/', { 0x01,0x02,0x02,0x04,0x08,0x08,0x10 } },
    { '+', { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 } },
    { '_', { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F } },
    { '>', { 0x10,0x08,0x04,0x02,0x04,0x08,0x10 } },
    { '<', { 0x01,0x02,0x04,0x08,0x04,0x02,0x01 } },
    { '%', { 0x19,0x19,0x02,0x04,0x08,0x13,0x13 } },
    { '#', { 0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x0A } },
    { '(', { 0x02,0x04,0x08,0x08,0x08,0x04,0x02 } },
    { ')', { 0x08,0x04,0x02,0x02,0x02,0x04,0x08 } },
    { '{', { 0x02,0x04,0x04,0x08,0x04,0x04,0x02 } },
    { '}', { 0x08,0x04,0x04,0x02,0x04,0x04,0x08 } },
    { '[', { 0x0E,0x08,0x08,0x08,0x08,0x08,0x0E } },
    { ']', { 0x0E,0x02,0x02,0x02,0x02,0x02,0x0E } },
    { ';', { 0x00,0x04,0x04,0x00,0x04,0x04,0x08 } },
    { '=', { 0x00,0x00,0x1F,0x00,0x1F,0x00,0x00 } },
    { '*', { 0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00 } },
    { '!', { 0x04,0x04,0x04,0x04,0x04,0x00,0x04 } },
    { '"', { 0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00 } },
    { '\'', { 0x04,0x04,0x04,0x00,0x00,0x00,0x00 } },
    { ',', { 0x00,0x00,0x00,0x00,0x0C,0x04,0x08 } },
};

static const u8 *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < ARRAY_SIZE(g_font); i++)
        if (g_font[i].ch == c)
            return g_font[i].row;
    return g_font[1].row;
}

bool gfx_available(void) { return g_ready; }

int gfx_init(void)
{
    if (g_ready)
        return FYX_OK;
    struct bga_info info;
    int rc = bga_init(&info);
    if (rc != 0)
        return rc;
    size_t pages = ALIGN_UP(info.size, PAGE_SIZE) / PAGE_SIZE;
    g_back_phys = pmm_alloc_frames(pages);
    if (g_back_phys == PMM_INVALID)
        return -FYX_ENOMEM;
    g_back = phys_to_virt(g_back_phys);
    memset(g_back, 0, pages * PAGE_SIZE);
    g_front = info.address;
    g_width = info.width;
    g_height = info.height;
    g_pitch = info.pitch;
    g_size = info.size;
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.width = g_width;
    g_stats.height = g_height;
    g_stats.backbuffer_phys = g_back_phys;
    g_ready = true;
    kprintf("[gfx] 软件背 buffer phys=0x%llx，%u KiB；像素格式 BGRX8888\n",
            (unsigned long long)g_back_phys,
            (unsigned)((pages * PAGE_SIZE) / 1024));
    return FYX_OK;
}

void gfx_pixel(i32 x, i32 y, u32 color)
{
    if (!g_ready || x < 0 || y < 0 || (u32)x >= g_width || (u32)y >= g_height)
        return;
    g_back[(size_t)y * (g_pitch / 4) + (u32)x] = color;
    g_stats.draw_calls++;
    g_stats.pixels_written++;
}

void gfx_clear(u32 color)
{
    if (!g_ready)
        return;
    u64 pair = (u64)color | ((u64)color << 32);
    u64 *p = (u64 *)g_back;
    for (size_t i = 0; i < g_size / 8; i++)
        p[i] = pair;
    g_stats.draw_calls++;
    g_stats.pixels_written += (u64)g_width * g_height;
}

void gfx_fill_rect(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    if (!g_ready || w <= 0 || h <= 0)
        return;
    i32 x0 = MAX(x, 0), y0 = MAX(y, 0);
    i32 x1 = MIN(x + w, (i32)g_width);
    i32 y1 = MIN(y + h, (i32)g_height);
    if (x0 >= x1 || y0 >= y1)
        return;
    for (i32 row = y0; row < y1; row++) {
        u32 *dst = g_back + (size_t)row * (g_pitch / 4);
        for (i32 col = x0; col < x1; col++)
            dst[col] = color;
    }
    g_stats.draw_calls++;
    g_stats.pixels_written += (u64)(x1 - x0) * (y1 - y0);
}

void gfx_draw_rect(i32 x, i32 y, i32 w, i32 h, u32 color)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_fill_round_rect(i32 x, i32 y, i32 w, i32 h, i32 radius, u32 color)
{
    if (!g_ready || w <= 0 || h <= 0)
        return;
    radius = MAX(radius, 0);
    radius = MIN(radius, MIN(w, h) / 2);
    u64 written = 0;
    for (i32 py = MAX(y, 0); py < MIN(y + h, (i32)g_height); py++) {
        for (i32 px = MAX(x, 0); px < MIN(x + w, (i32)g_width); px++) {
            bool inside = true, corner = false;
            i32 cx = 0, cy = 0;
            if (px < x + radius && py < y + radius) {
                cx = x + radius; cy = y + radius; corner = true;
            } else if (px >= x + w - radius && py < y + radius) {
                cx = x + w - radius - 1; cy = y + radius; corner = true;
            } else if (px < x + radius && py >= y + h - radius) {
                cx = x + radius; cy = y + h - radius - 1; corner = true;
            } else if (px >= x + w - radius && py >= y + h - radius) {
                cx = x + w - radius - 1; cy = y + h - radius - 1; corner = true;
            }
            if (corner) {
                i32 dx = px - cx, dy = py - cy;
                inside = dx * dx + dy * dy <= radius * radius;
            }
            if (inside) {
                g_back[(size_t)py * (g_pitch / 4) + px] = color;
                written++;
            }
        }
    }
    g_stats.draw_calls++;
    g_stats.pixels_written += written;
}

void gfx_fill_circle(i32 cx, i32 cy, i32 radius, u32 color)
{
    if (!g_ready || radius <= 0)
        return;
    for (i32 dy = -radius; dy <= radius; dy++)
        for (i32 dx = -radius; dx <= radius; dx++)
            if (dx * dx + dy * dy <= radius * radius)
                gfx_pixel(cx + dx, cy + dy, color);
}

void gfx_draw_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 color)
{
    if (!g_ready)
        return;
    i32 dx = x1 > x0 ? x1 - x0 : x0 - x1;
    i32 sx = x0 < x1 ? 1 : -1;
    i32 dy = y1 > y0 ? y0 - y1 : y1 - y0;
    i32 sy = y0 < y1 ? 1 : -1;
    i32 error = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        i32 twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

void gfx_draw_text(i32 x, i32 y, const char *text, u32 color, u32 scale)
{
    if (!g_ready || !text || !scale)
        return;
    i32 pen = x;
    for (size_t i = 0; text[i]; i++) {
        char c = text[i];
        if (c == '\n') {
            pen = x;
            y += (i32)(8 * scale);
            continue;
        }
        const u8 *rows = glyph_for(c);
        for (u32 row = 0; row < 7; row++)
            for (u32 col = 0; col < 5; col++)
                if (rows[row] & (1u << (4 - col)))
                    gfx_fill_rect(pen + (i32)(col * scale),
                                  y + (i32)(row * scale),
                                  (i32)scale, (i32)scale, color);
        pen += (i32)(6 * scale);
    }
}

u32 gfx_text_width(const char *text, u32 scale)
{
    if (!text || !scale)
        return 0;
    size_t n = 0, longest = 0, current = 0;
    for (; text[n]; n++) {
        if (text[n] == '\n') {
            longest = MAX(longest, current);
            current = 0;
        } else {
            current++;
        }
    }
    longest = MAX(longest, current);
    return (u32)(longest * 6 * scale);
}

u32 gfx_checksum(void)
{
    if (!g_ready)
        return 0;
    const volatile u8 *bytes = (const volatile u8 *)g_front;
    u32 hash = 2166136261u;
    for (size_t i = 0; i < g_size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

u32 gfx_present(void)
{
    if (!g_ready)
        return 0;
    for (u32 row = 0; row < g_height; row++) {
        const u64 *src = (const u64 *)((const u8 *)g_back + (size_t)row * g_pitch);
        volatile u64 *dst = (volatile u64 *)((volatile u8 *)g_front +
                                             (size_t)row * g_pitch);
        for (u32 col = 0; col < g_pitch / 8; col++)
            dst[col] = src[col];
    }
    g_stats.frames++;
    g_stats.blit_bytes += g_size;
    g_stats.last_checksum = gfx_checksum();
    return g_stats.last_checksum;
}

void gfx_get_stats(struct gfx_stats *out)
{
    if (!out)
        return;
    *out = g_stats;
    out->width = g_width;
    out->height = g_height;
    out->backbuffer_phys = g_back_phys;
}
