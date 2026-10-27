/*
 * FYXOS — 8253/8254 可编程定时器（系统心跳）
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/sched.h>

#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43

#define PIT_MODE_RATE   0x04     /* 方式 2：速率发生器 */
#define PIT_LOBYTE_HIBYTE 0x30

static u64 g_ticks;
static u32 g_frequency;
static bool g_running;
static int  g_irq_line = -1;

static void timer_handler(void *arg)
{
    UNUSED(arg);
    g_ticks++;
    sched_tick();
    irq_eoi(IRQ_TIMER);
}

void pit_init(u32 frequency_hz)
{
    if (frequency_hz == 0 || frequency_hz > PIT_HZ)
        frequency_hz = 100;

    g_frequency = frequency_hz;
    g_ticks = 0;

    u32 divisor = PIT_HZ / frequency_hz;
    if (divisor == 0 || divisor > 0xFFFF)
        divisor = 0xFFFF;

    outb(PIT_COMMAND, PIT_MODE_RATE | PIT_LOBYTE_HIBYTE);
    io_wait();
    outb(PIT_CHANNEL0, (u8)(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0, (u8)((divisor >> 8) & 0xFF));
    io_wait();

    if (irq_register(IRQ_TIMER, timer_handler, "pit", NULL) == 0) {
        g_irq_line = IRQ_TIMER;
        pic_unmask(IRQ_TIMER);
        g_running = true;
    }
}

u64 pit_ticks(void)
{
    return g_ticks;
}

u32 pit_frequency(void)
{
    return g_frequency;
}

void pit_stop(void)
{
    if (g_irq_line >= 0) {
        pic_mask((u8)g_irq_line);
        irq_unregister((u8)g_irq_line);
        g_irq_line = -1;
    }
    g_running = false;
}
