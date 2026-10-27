/*
 * FYXOS — 16550 UART 串口驱动（COM1/COM2）
 * 轮询模式；中断模式留到阶段 2 的驱动框架里再加。
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>

#define SER_DATA(base)        (base + 0)
#define SER_INT_ENABLE(base)  (base + 1)
#define SER_FIFO_CTRL(base)   (base + 2)
#define SER_LINE_CTRL(base)   (base + 3)
#define SER_MODEM_CTRL(base)  (base + 4)
#define SER_LINE_STATUS(base) (base + 5)

#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

static u16 g_console_port = COM1_BASE;
static bool g_initialized;

void serial_init(u16 port)
{
    g_console_port = port;

    outb(SER_INT_ENABLE(port), 0x00);     /* 关闭所有串口中断 */
    outb(SER_LINE_CTRL(port), 0x80);      /* DLAB = 1，设置波特率 */
    outb(SER_DATA(port), 0x01);           /* 除数 1 -> 115200 bps */
    outb(SER_INT_ENABLE(port), 0x00);
    outb(SER_LINE_CTRL(port), 0x03);      /* 8 位、无校验、1 停止位 */
    outb(SER_FIFO_CTRL(port), 0xC7);      /* 使能 FIFO，14 字节触发阈值 */
    outb(SER_MODEM_CTRL(port), 0x0B);     /* RTS/DTR 置位，辅助输出就绪 */

    g_initialized = true;
}

int serial_received(u16 port)
{
    return (inb(SER_LINE_STATUS(port)) & LSR_DATA_READY) != 0;
}

char serial_read(u16 port)
{
    while (!serial_received(port))
        cpu_pause();
    return (char)inb(SER_DATA(port));
}

void serial_write(u16 port, char c)
{
    if (!g_initialized && port == g_console_port) {
        /* 未初始化时也尽量输出，保证 early boot 信息不丢 */
    }
    /* 等待发送保持寄存器空。
     * 带超时：某些模拟器上 LSR 的 THRE 位可能不会翻转。 */
    u32 spins = 0;
    while ((inb(SER_LINE_STATUS(port)) & LSR_THR_EMPTY) == 0) {
        if (++spins > 0x100000)
            break;
        cpu_pause();
    }
    outb(SER_DATA(port), (u8)c);
}

void serial_print(u16 port, const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_write(port, '\r');
        serial_write(port, *s++);
    }
}
