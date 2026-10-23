/*
 * FYXOS — 内核日志与控制台
 * ---------------------------------------------------------------------------
 * 输出流水线：
 *
 *   console_putc(c)
 *        ├─> 内核日志环形缓冲（**总是**写入，物理内存常驻）
 *        ├─> 16550 串口（若已注册）
 *        └─> VGA 文本帧缓冲（若已注册）
 *
 * 日志缓冲放在 .data 段，且刻意不使用 .bss：它是引导器与调试器在
 * 内核完全启动前/崩溃后都能读到的“黑匣子”。
 */
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/x86_64.h>
#include <fyxos/sync.h>

struct fyx_log g_klog = {
    .magic  = FYX_LOG_MAGIC,
    .size   = FYX_LOG_SIZE,
    .head   = 0,
    .dropped = 0,
    .write_count = 0,
    .panic_flag = 0,
};

static u32     g_backends = CONSOLE_LOG;
static bool    g_serial_ready;
static bool    g_vga_ready;
static spinlock_t g_console_lock = SPINLOCK_INIT("console");

void log_init(void)
{
    g_klog.magic = FYX_LOG_MAGIC;
    g_klog.size = FYX_LOG_SIZE;
    g_klog.head = 0;
    g_klog.write_count = 0;
    g_klog.panic_flag = 0;
    memset(g_klog.buf, 0, FYX_LOG_SIZE);
}

void log_putc(char c)
{
    if (g_klog.magic != FYX_LOG_MAGIC)
        return;
    g_klog.buf[g_klog.head % FYX_LOG_SIZE] = c;
    g_klog.head++;
    g_klog.write_count++;
    /* 用编译器屏障保证 head 的推进对外部观察者（调试器/台架）可见 */
    barrier();
}

void log_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        log_putc(s[i]);
}

void console_register_backends(u32 mask)
{
    g_backends = mask | CONSOLE_LOG;
    g_serial_ready = !!(mask & CONSOLE_SERIAL);
    g_vga_ready = !!(mask & CONSOLE_VGA);
}

void console_init(void)
{
    log_init();
    g_backends = CONSOLE_LOG;
    g_serial_ready = false;
    g_vga_ready = false;
}

void console_putc(char c)
{
    /* 换行统一转成 CRLF，串口终端与 VGA 都能正确显示 */
    if (c == '\n') {
        log_putc('\r');
        if (g_serial_ready)
            serial_write(COM1_BASE, '\r');
        if (g_vga_ready)
            vga_putc('\r');
    }

    log_putc(c);
    if (g_serial_ready)
        serial_write(COM1_BASE, c);
    if (g_vga_ready)
        vga_putc(c);
}

void console_write(const char *s, size_t n)
{
    u64 flags = irq_save_disable();
    spin_lock(&g_console_lock);
    for (size_t i = 0; i < n; i++)
        console_putc(s[i]);
    spin_unlock(&g_console_lock);
    irq_restore(flags);
}

void console_set_color(u8 fg, u8 bg)
{
    if (g_vga_ready)
        vga_set_color(fg, bg);
}
