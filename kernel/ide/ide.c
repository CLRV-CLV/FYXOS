/* FYXOS — FYX Studio：真实文本缓冲 + 语法视图 + 原生构建/运行 */
#include <fyxos/ide.h>
#include <fyxos/gfx.h>
#include <fyxos/fyxcc.h>
#include <fyxos/process.h>
#include <fyxos/vfs.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/x86_64.h>

#define IDE_BUFFER_CAP 16384u
#define IDE_VISIBLE_LINES 25u
#define IDE_CTRL_B 0x02
#define IDE_CTRL_Q 0x11
#define IDE_CTRL_R 0x12
#define IDE_CTRL_S 0x13

static char *g_buffer;
static size_t g_length;
static char g_path[VFS_PATH_MAX];
static char g_output[VFS_PATH_MAX];
static char g_status[128];
static bool g_dirty;
static bool g_built;
static bool g_insert;
static bool g_open;

static u32 ide_color_text(void) { return GFX_RGB(220, 229, 247); }

static void ide_set_status(const char *text)
{
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
}

static void ide_make_output_path(void)
{
    size_t length = strlen(g_path), dot = length, slash = 0;
    for (size_t i = 0; i < length; i++) {
        if (g_path[i] == '/')
            slash = i + 1;
        if (g_path[i] == '.')
            dot = i;
    }
    if (dot < slash)
        dot = length;
    if (dot + 5 >= sizeof(g_output)) {
        g_output[0] = '\0';
        return;
    }
    memcpy(g_output, g_path, dot);
    memcpy(g_output + dot, ".elf", 5);
}

static bool ide_word(const char *line, size_t start, size_t length,
                     const char *word)
{
    return strlen(word) == length && memcmp(line + start, word, length) == 0;
}

static bool ide_keyword(const char *line, size_t start, size_t length)
{
    return ide_word(line, start, length, "int") ||
           ide_word(line, start, length, "main") ||
           ide_word(line, start, length, "void") ||
           ide_word(line, start, length, "return") ||
           ide_word(line, start, length, "puts") ||
           ide_word(line, start, length, "print");
}

static void ide_draw_code_line(const char *line, size_t length, i32 x, i32 y)
{
    bool string = false, comment = false;
    size_t i = 0;
    while (i < length) {
        if (!string && i + 1 < length && line[i] == '/' && line[i + 1] == '/')
            comment = true;
        if (isalpha(line[i]) && !string && !comment) {
            size_t start = i;
            while (i < length && (isalpha(line[i]) || line[i] == '_'))
                i++;
            u32 color = ide_keyword(line, start, i - start)
                ? GFX_RGB(103, 211, 255) : ide_color_text();
            for (size_t j = start; j < i; j++) {
                char one[2] = { line[j], 0 };
                gfx_draw_text(x + (i32)j * 6, y, one, color, 1);
            }
            continue;
        }
        char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\') && !comment)
            string = !string;
        u32 color = comment ? GFX_RGB(112, 133, 158)
            : string ? GFX_RGB(190, 143, 255) : ide_color_text();
        char one[2] = { c, 0 };
        gfx_draw_text(x + (i32)i * 6, y, one, color, 1);
        i++;
    }
}

static void ide_cursor(u32 visible_line)
{
    if (!g_insert)
        return;
    size_t line = 0, column = 0;
    for (size_t i = 0; i < g_length; i++) {
        if (g_buffer[i] == '\n') {
            line++;
            column = 0;
        } else {
            column++;
        }
    }
    if (line == visible_line || line < IDE_VISIBLE_LINES) {
        if (line < IDE_VISIBLE_LINES)
            gfx_fill_rect(168 + (i32)column * 6, 100 + (i32)line * 17,
                          2, 10, GFX_RGB(103, 211, 255));
    }
}

static void ide_render(void)
{
    if (!g_open || !gfx_available())
        return;
    const u32 bg = GFX_RGB(10, 17, 32), panel = GFX_RGB(19, 29, 49);
    const u32 muted = GFX_RGB(137, 157, 187), accent = GFX_RGB(103, 211, 255);
    gfx_clear(bg);
    gfx_fill_rect(0, 0, 800, 44, panel);
    gfx_fill_rect(0, 43, 800, 1, accent);
    gfx_fill_round_rect(16, 9, 26, 26, 6, accent);
    gfx_draw_text(21, 14, "FX", panel, 2);
    gfx_draw_text(58, 14, "FYX STUDIO", ide_color_text(), 2);
    gfx_draw_text(286, 16, g_path, muted, 1);
    gfx_draw_text(560, 16, g_insert ? "INSERT" : "COMMAND", accent, 1);

    gfx_fill_round_rect(12, 56, 128, 474, 10, panel);
    gfx_draw_text(26, 72, "WORKSPACE", accent, 1);
    gfx_fill_rect(24, 88, 104, 1, GFX_RGB(46, 64, 91));
    gfx_draw_text(26, 106, "PROJECT", muted, 1);
    gfx_draw_text(26, 132, "> SOURCE", ide_color_text(), 1);
    gfx_draw_text(26, 158, "  MAIN.C", accent, 1);
    gfx_draw_text(26, 194, "BUILD", muted, 1);
    gfx_draw_text(26, 220, g_built ? "  ELF READY" : "  NOT BUILT",
                  g_built ? GFX_RGB(108, 224, 172) : muted, 1);
    gfx_draw_text(26, 258, "TARGET", muted, 1);
    gfx_draw_text(26, 284, "  X86_64", ide_color_text(), 1);
    gfx_draw_text(26, 310, "  FYXOS", ide_color_text(), 1);
    gfx_draw_text(26, 356, "NATIVE", accent, 1);
    gfx_draw_text(26, 376, "NO WEB", muted, 1);
    gfx_draw_text(26, 396, "RUNTIME", muted, 1);
    gfx_draw_text(26, 474, g_dirty ? "MODIFIED" : "SAVED",
                  g_dirty ? GFX_RGB(255, 188, 105) : muted, 1);

    gfx_fill_round_rect(148, 56, 640, 474, 10, GFX_RGB(14, 23, 40));
    gfx_draw_text(166, 72, g_status, muted, 1);
    size_t start = 0;
    u32 row = 0;
    while (row < IDE_VISIBLE_LINES) {
        size_t end = start;
        while (end < g_length && g_buffer[end] != '\n')
            end++;
        char number[8];
        snprintf(number, sizeof(number), "%2u", row + 1);
        gfx_draw_text(156, 100 + (i32)row * 17, number, muted, 1);
        if (end > start)
            ide_draw_code_line(g_buffer + start, end - start, 184,
                               100 + (i32)row * 17);
        if (end == g_length)
            break;
        start = end + 1;
        row++;
    }
    ide_cursor(row);

    gfx_fill_rect(0, 544, 800, 56, panel);
    gfx_fill_rect(0, 544, 800, 1, GFX_RGB(46, 64, 91));
    gfx_draw_text(18, 557, "I INSERT   ESC COMMAND   CTRL-S SAVE   CTRL-B BUILD",
                  ide_color_text(), 1);
    gfx_draw_text(18, 578, "CTRL-R RUN   CTRL-Q QUIT   NATIVE C SUBSET -> ELF -> RING3",
                  accent, 1);
    gfx_present();
}

int ide_open(const char *path)
{
    if (!path || !*path || strlen(path) >= sizeof(g_path))
        return -FYX_EINVAL;
    if (!g_buffer) {
        g_buffer = kmalloc(IDE_BUFFER_CAP);
        if (!g_buffer)
            return -FYX_ENOMEM;
    }
    void *file = NULL;
    size_t size = 0;
    int rc = vfs_read_file(path, &file, &size);
    if (rc != 0 && rc != -FYX_ENOENT) {
        return rc;
    }
    if (rc == 0 && size >= IDE_BUFFER_CAP) {
        kfree(file);
        return -FYX_ERANGE;
    }
    memset(g_buffer, 0, IDE_BUFFER_CAP);
    if (rc == 0 && size) {
        memcpy(g_buffer, file, size);
        g_length = size;
    } else {
        g_length = 0;
    }
    kfree(file);
    strncpy(g_path, path, sizeof(g_path) - 1);
    g_path[sizeof(g_path) - 1] = '\0';
    ide_make_output_path();
    if (!g_output[0])
        return -FYX_ERANGE;
    g_dirty = rc != 0;
    g_built = false;
    g_insert = false;
    g_open = true;
    ide_set_status(rc == 0 ? "READY / SOURCE LOADED" : "NEW FILE / TYPE SOURCE");
    return FYX_OK;
}

static int ide_save(void)
{
    int rc = vfs_write_file(g_path, g_buffer, g_length);
    if (rc == 0) {
        g_dirty = false;
        ide_set_status("SAVED / SOURCE ON FYXFS");
        kprintf("[ide] saved %s（%u 字节）\n", g_path, (unsigned)g_length);
    } else {
        ide_set_status("SAVE ERROR");
        kprintf("[ide] save failed: %d\n", rc);
    }
    return rc;
}

static int ide_build(void)
{
    if (g_dirty && ide_save() != 0)
        return -FYX_EIO;
    struct fyxcc_diagnostic diagnostic;
    void *image = NULL;
    size_t image_size = 0;
    int rc = fyxcc_compile(g_buffer, g_length, &image, &image_size, &diagnostic);
    if (rc != 0) {
        ide_set_status("BUILD ERROR / SEE SERIAL");
        kprintf("[ide] compile error at %u: %s\n",
                (unsigned)diagnostic.offset,
                diagnostic.message[0] ? diagnostic.message : "invalid source");
        return rc;
    }
    rc = vfs_write_file(g_output, image, image_size);
    kfree(image);
    if (rc != 0) {
        ide_set_status("ELF WRITE ERROR");
        return rc;
    }
    g_built = true;
    ide_set_status("BUILD OK / NATIVE X86_64 ELF");
    kprintf("[ide] built %s（%u 字节）\n", g_output, (unsigned)image_size);
    return FYX_OK;
}

static int ide_run_program(void)
{
    if (!g_built || g_dirty) {
        int rc = ide_build();
        if (rc != 0)
            return rc;
    }
    int status = 0;
    int rc = process_run_wait(g_output, &status);
    if (rc != 0) {
        ide_set_status("RUN ERROR");
        kprintf("[ide] run failed: %d\n", rc);
        return rc;
    }
    char message[64];
    snprintf(message, sizeof(message), "RUN COMPLETE / EXIT %d", status);
    ide_set_status(message);
    kprintf("[ide] program exit=%d\n", status);
    return status;
}

int ide_run(void)
{
    if (!g_open || !g_buffer)
        return -FYX_EINVAL;
    int rc = gfx_init();
    if (rc != 0)
        return rc;
    ide_render();
    kprintf("[ide] ready %s；I 插入，Esc 命令态，Ctrl-S/B/R/Q 保存/编译/运行/退出\n",
            g_path);
    for (;;) {
        char c = keyboard_getchar();
        if (c == IDE_CTRL_Q)
            break;
        if (c == IDE_CTRL_S) {
            ide_save();
            ide_render();
            continue;
        }
        if (c == IDE_CTRL_B) {
            ide_build();
            ide_render();
            continue;
        }
        if (c == IDE_CTRL_R) {
            ide_run_program();
            ide_render();
            continue;
        }
        if (c == 27) {
            g_insert = false;
            ide_render();
            continue;
        }
        if (!g_insert && c == 'i') {
            g_insert = true;
            ide_render();
            continue;
        }
        if (!g_insert)
            continue;
        if (c == '\b' || c == 0x7F) {
            if (g_length) {
                g_length--;
                g_buffer[g_length] = '\0';
                g_dirty = true;
                g_built = false;
            }
        } else if (c == '\n' || c == '\r' || isprint(c)) {
            if (g_length + 1 >= IDE_BUFFER_CAP) {
                ide_set_status("BUFFER FULL");
                continue;
            }
            g_buffer[g_length++] = c == '\r' ? '\n' : c;
            g_buffer[g_length] = '\0';
            g_dirty = true;
            g_built = false;
        }
        ide_render();
    }
    struct gfx_stats stats;
    gfx_get_stats(&stats);
    kprintf("[ide] closed frames=%llu checksum=0x%08x\n",
            (unsigned long long)stats.frames, stats.last_checksum);
    g_open = false;
    return FYX_OK;
}
