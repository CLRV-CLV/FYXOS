/* FYXOS — 三窗口、焦点/z-order、双主题的软件合成桌面 */
#include <fyxos/desktop.h>
#include <fyxos/gfx.h>
#include <fyxos/net.h>
#include <fyxos/vfs.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/x86_64.h>

struct desktop_window {
    i32 x, y, w, h;
    const char *title;
    u32 id;
};

struct palette {
    u32 wall_top, wall_bottom, decoration;
    u32 panel, window, window_muted, text, muted, accent, accent2, shadow;
};

static struct desktop_window g_windows[] = {
    { 54, 86, 424, 260, "WELCOME", 0 },
    { 490, 112, 260, 206, "NETWORK", 1 },
    { 260, 310, 450, 180, "RESTORE", 2 },
};
static u32 g_focus;
static u32 g_theme;
static bool g_ready;

static const struct palette g_dark = {
    .wall_top = GFX_RGB(12, 20, 43), .wall_bottom = GFX_RGB(42, 24, 70),
    .decoration = GFX_RGB(29, 42, 79), .panel = GFX_RGB(17, 25, 48),
    .window = GFX_RGB(25, 34, 59), .window_muted = GFX_RGB(42, 54, 82),
    .text = GFX_RGB(239, 244, 255), .muted = GFX_RGB(163, 177, 207),
    .accent = GFX_RGB(100, 210, 255), .accent2 = GFX_RGB(178, 132, 255),
    .shadow = GFX_RGB(7, 11, 25),
};
static const struct palette g_light = {
    .wall_top = GFX_RGB(218, 235, 252), .wall_bottom = GFX_RGB(232, 215, 246),
    .decoration = GFX_RGB(193, 215, 241), .panel = GFX_RGB(245, 248, 253),
    .window = GFX_RGB(252, 253, 255), .window_muted = GFX_RGB(226, 233, 244),
    .text = GFX_RGB(28, 39, 61), .muted = GFX_RGB(83, 99, 126),
    .accent = GFX_RGB(17, 125, 184), .accent2 = GFX_RGB(121, 75, 181),
    .shadow = GFX_RGB(159, 174, 198),
};

static const struct palette *current_palette(void)
{
    return g_theme ? &g_light : &g_dark;
}

static void draw_background(const struct palette *p, u32 width, u32 height)
{
    for (u32 y = 0; y < height; y++) {
        u32 r0 = (p->wall_top >> 16) & 0xFF, g0 = (p->wall_top >> 8) & 0xFF;
        u32 b0 = p->wall_top & 0xFF;
        u32 r1 = (p->wall_bottom >> 16) & 0xFF, g1 = (p->wall_bottom >> 8) & 0xFF;
        u32 b1 = p->wall_bottom & 0xFF;
        u32 r = r0 + (r1 > r0 ? (r1 - r0) * y / height : 0);
        u32 g = g0 + (g1 > g0 ? (g1 - g0) * y / height : 0);
        u32 b = b0 + (b1 > b0 ? (b1 - b0) * y / height : 0);
        /* 也支持反向渐变，避免调色板换成浅色后通道停在上界。 */
        if (r1 < r0) r = r0 - (r0 - r1) * y / height;
        if (g1 < g0) g = g0 - (g0 - g1) * y / height;
        if (b1 < b0) b = b0 - (b0 - b1) * y / height;
        gfx_fill_rect(0, (i32)y, (i32)width, 1, GFX_RGB(r, g, b));
    }
    gfx_fill_circle((i32)width - 40, 64, 170, p->decoration);
    gfx_fill_circle(38, (i32)height - 20, 142, p->decoration);
    gfx_draw_line(0, 330, (i32)width, 190, p->window_muted);
}

static void draw_topbar(const struct palette *p, u32 width)
{
    gfx_fill_rect(0, 0, (i32)width, 44, p->panel);
    gfx_fill_rect(0, 43, (i32)width, 1, p->accent);
    gfx_fill_round_rect(18, 9, 26, 26, 7, p->accent);
    gfx_draw_text(23, 14, "FX", p->panel, 2);
    gfx_draw_text(60, 14, "FYXOS", p->text, 2);
    gfx_draw_text(286, 16, "DESKTOP / SYSTEM OVERVIEW", p->muted, 1);
    gfx_draw_text((i32)width - 82, 14, "10:24", p->text, 2);
}

static void format_ip(char *buf, size_t size, u32 ip)
{
    snprintf(buf, size, "%u.%u.%u.%u", (ip >> 24) & 0xFF,
             (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

static void draw_window_content(const struct desktop_window *w,
                                const struct palette *p)
{
    i32 x = w->x + 22, y = w->y + 60;
    char line[96];
    if (w->id == 0) {
        gfx_draw_text(x, y, "WELCOME TO FYXOS", p->text, 2);
        gfx_draw_text(x, y + 30, "A SMALL FAST SYSTEM", p->muted, 1);
        gfx_fill_round_rect(x, y + 52, 174, 58, 8, p->window_muted);
        gfx_fill_round_rect(x + 186, y + 52, 174, 58, 8, p->window_muted);
        gfx_draw_text(x + 14, y + 64, "38 SYSCALLS", p->accent, 2);
        gfx_draw_text(x + 200, y + 64, "66 COMMANDS", p->accent2, 2);
        gfx_draw_text(x + 14, y + 88, "RING3 + FD ABI", p->text, 1);
        gfx_draw_text(x + 200, y + 88, "COW RESTORE", p->text, 1);
        gfx_draw_text(x, y + 132, "FYXFS / HOME PRESERVED", p->muted, 1);
        gfx_draw_text(x, y + 153, "N NEXT   T THEME   R REDRAW", p->accent, 1);
    } else if (w->id == 1) {
        struct net_info info;
        struct net_stats stats;
        net_get_info(&info);
        net_get_stats(&stats);
        gfx_draw_text(x, y, "RTL8139 / IPV4", p->text, 2);
        gfx_draw_text(x, y + 30, info.ready ? "DHCP ONLINE" : "OFFLINE",
                      info.ready ? p->accent : p->muted, 1);
        format_ip(line, sizeof(line), info.ip);
        gfx_draw_text(x, y + 50, line, p->text, 2);
        snprintf(line, sizeof(line), "UDP %llu / %llu",
                 (unsigned long long)stats.udp_received,
                 (unsigned long long)stats.udp_sent);
        gfx_draw_text(x, y + 78, line, p->muted, 1);
        gfx_draw_text(x, y + 99, "ARP + ICMP READY", p->accent2, 1);
        gfx_draw_text(x, y + 120, "SOCKET FD ACTIVE", p->muted, 1);
    } else {
        struct fyxfs_stats stats;
        vfs_stats(&stats);
        gfx_draw_text(x, y, "COW RESTORE", p->text, 2);
        snprintf(line, sizeof(line), "GENERATION %llu",
                 (unsigned long long)stats.generation);
        gfx_draw_text(x, y + 30, line, p->muted, 1);
        snprintf(line, sizeof(line), "%u SNAPSHOTS / FSCK READY", stats.snapshots);
        gfx_draw_text(x, y + 51, line, p->accent, 1);
        gfx_draw_text(x, y + 75, "SYSTEM ROLLBACK", p->text, 1);
        gfx_draw_text(x, y + 96, "/HOME PRESERVED", p->accent2, 1);
    }
}

static void draw_window(const struct desktop_window *w, const struct palette *p,
                        bool focused)
{
    gfx_fill_round_rect(w->x + 8, w->y + 10, w->w, w->h, 14, p->shadow);
    if (focused)
        gfx_fill_round_rect(w->x - 2, w->y - 2, w->w + 4, w->h + 4,
                            16, p->accent);
    gfx_fill_round_rect(w->x, w->y, w->w, w->h, 14, p->window);
    gfx_draw_text(w->x + 18, w->y + 13, w->title, p->text, 2);
    gfx_fill_circle(w->x + w->w - 24, w->y + 20, 5,
                    focused ? p->accent : p->window_muted);
    gfx_fill_rect(w->x + 18, w->y + 40, w->w - 36, 1, p->window_muted);
    draw_window_content(w, p);
}

static void draw_dock(const struct palette *p, u32 width)
{
    i32 x = (i32)width / 2 - 230;
    gfx_fill_round_rect(x, 534, 460, 52, 16, p->panel);
    static const char *const labels[] = { "HOME", "TERM", "NET", "SNAP" };
    static const u32 colors[] = {
        GFX_RGB(100,210,255), GFX_RGB(178,132,255),
        GFX_RGB(108,224,172), GFX_RGB(255,188,105)
    };
    for (u32 i = 0; i < 4; i++) {
        i32 ix = x + 20 + (i32)i * 112;
        gfx_fill_circle(ix + 12, 557, 11, colors[i]);
        gfx_draw_text(ix + 30, 551, labels[i], p->text, 1);
    }
}

static int ensure_desktop(void)
{
    if (g_ready)
        return FYX_OK;
    int rc = gfx_init();
    if (rc != 0)
        return rc;
    g_focus = 0;
    g_theme = 0;
    g_ready = true;
    return FYX_OK;
}

static int desktop_render(void)
{
    int rc = ensure_desktop();
    if (rc != 0)
        return rc;
    struct gfx_stats stats;
    gfx_get_stats(&stats);
    const struct palette *p = current_palette();
    draw_background(p, stats.width, stats.height);
    draw_topbar(p, stats.width);
    for (u32 i = 0; i < ARRAY_SIZE(g_windows); i++)
        if (i != g_focus)
            draw_window(&g_windows[i], p, false);
    draw_window(&g_windows[g_focus], p, true);
    draw_dock(p, stats.width);
    gfx_draw_text(20, 580, "N NEXT   T THEME   R REDRAW   Q SHELL", p->text, 1);
    u32 checksum = gfx_present();
    kprintf("[desktop] frame=%llu focus=%s theme=%s checksum=0x%08x\n",
            (unsigned long long)stats.frames + 1, g_windows[g_focus].title,
            g_theme ? "light" : "dark", checksum);
    return FYX_OK;
}

int desktop_start(void)
{
    return desktop_render();
}

int desktop_next_window(void)
{
    int rc = ensure_desktop();
    if (rc != 0)
        return rc;
    g_focus = (g_focus + 1) % ARRAY_SIZE(g_windows);
    return desktop_render();
}

int desktop_toggle_theme(void)
{
    int rc = ensure_desktop();
    if (rc != 0)
        return rc;
    g_theme ^= 1;
    return desktop_render();
}

void desktop_print_info(void)
{
    if (!g_ready) {
        kprintf("desktop: 尚未启动，执行 desktop 启动\n");
        return;
    }
    struct gfx_stats stats;
    gfx_get_stats(&stats);
    kprintf("[desktop] windows=%u focus=%s theme=%s frames=%llu "
            "blit=%llu KiB checksum=0x%08x\n",
            (unsigned)ARRAY_SIZE(g_windows), g_windows[g_focus].title,
            g_theme ? "light" : "dark", (unsigned long long)stats.frames,
            (unsigned long long)(stats.blit_bytes / 1024), stats.last_checksum);
    kprintf("  背 buffer phys=0x%llx，绘制调用=%llu，像素=%llu\n",
            (unsigned long long)stats.backbuffer_phys,
            (unsigned long long)stats.draw_calls,
            (unsigned long long)stats.pixels_written);
}

int desktop_run(void)
{
    int rc = desktop_start();
    if (rc != 0)
        return rc;
    kprintf("[desktop] 交互：N 下一窗口，T 主题，R 重绘，Q/Esc 返回 Shell\n");
    for (;;) {
        char c = keyboard_getchar();
        if (c == 'q' || c == 'Q' || c == 27)
            break;
        if (c == 'n' || c == 'N' || c == '\t')
            desktop_next_window();
        else if (c == 't' || c == 'T')
            desktop_toggle_theme();
        else if (c == 'r' || c == 'R')
            desktop_render();
    }
    desktop_print_info();
    return FYX_OK;
}
