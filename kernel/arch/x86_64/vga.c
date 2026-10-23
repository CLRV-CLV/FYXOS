/*
 * FYXOS — VGA 文本模式控制台（80x25）
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>

#define VGA_COLOR(fg, bg) ((u8)(((bg) << 4) | ((fg) & 0x0F)))

static volatile u16 *g_vga;
static u16 g_row, g_col;
static u8  g_attr = 0x0F;         /* 默认：白字黑底 */
static bool g_ready;

#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

static void vga_move_cursor(u16 row, u16 col)
{
    u16 pos = row * VGA_WIDTH + col;
    outb(CRTC_INDEX, 0x0F);
    outb(CRTC_DATA, (u8)(pos & 0xFF));
    outb(CRTC_INDEX, 0x0E);
    outb(CRTC_DATA, (u8)((pos >> 8) & 0xFF));
}

void vga_init(void)
{
    g_vga = (volatile u16 *)phys_to_virt(VGA_MEMORY);
    g_row = 0;
    g_col = 0;
    g_attr = VGA_COLOR(7, 0);
    g_ready = true;
    vga_clear();
}

void vga_clear(void)
{
    if (!g_ready)
        return;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        g_vga[i] = ((u16)g_attr << 8) | ' ';
    g_row = 0;
    g_col = 0;
    vga_move_cursor(g_row, g_col);
}

static void vga_scroll(void)
{
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++)
            g_vga[(row - 1) * VGA_WIDTH + col] = g_vga[row * VGA_WIDTH + col];
    }
    for (int col = 0; col < VGA_WIDTH; col++)
        g_vga[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = ((u16)g_attr << 8) | ' ';
    g_row = VGA_HEIGHT - 1;
}

void vga_putc(char c)
{
    if (!g_ready)
        return;

    switch (c) {
    case '\n':
        g_row++;
        g_col = 0;
        break;
    case '\r':
        g_col = 0;
        break;
    case '\t':
        g_col = (g_col + 8) & ~7u;
        break;
    case '\b':
        if (g_col > 0)
            g_col--;
        break;
    default:
        g_vga[g_row * VGA_WIDTH + g_col] = ((u16)g_attr << 8) | (u8)c;
        g_col++;
        break;
    }

    if (g_col >= VGA_WIDTH) {
        g_col = 0;
        g_row++;
    }
    if (g_row >= VGA_HEIGHT)
        vga_scroll();

    vga_move_cursor(g_row, g_col);
}

void vga_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        vga_putc(s[i]);
}

void vga_set_color(u8 fg, u8 bg)
{
    g_attr = VGA_COLOR(fg, bg);
}

void vga_get_cursor(u16 *row, u16 *col)
{
    if (row) *row = g_row;
    if (col) *col = g_col;
}

void vga_set_cursor(u16 row, u16 col)
{
    g_row = row;
    g_col = col;
    vga_move_cursor(g_row, g_col);
}
