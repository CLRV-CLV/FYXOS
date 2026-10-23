/*
 * FYXOS — panic / 停机 / 关停
 */
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/x86_64.h>

struct panic_record g_panic_record;

void NORETURN arch_shutdown(u8 code)
{
    /* 0x501 是 QEMU 的 isa-debug-exit 约定端口，fyxemu 也监听它 */
    outb(FYX_DEBUG_PORT, code);
    io_wait();
    for (;;) {
        disable_interrupts();
        cpu_halt();
    }
}

void arch_reboot(void)
{
    disable_interrupts();
    /* 8042 键盘控制器复位线 */
    outb(0x64, 0xFE);
    io_wait();
    for (;;)
        cpu_halt();
}

void kernel_halt(void)
{
    disable_interrupts();
    for (;;)
        cpu_halt();
}

static void dump_registers(void)
{
    kprintf("  RIP=%016llx  RSP=%016llx\n",
            (unsigned long long)g_panic_record.rip,
            (unsigned long long)g_panic_record.rsp);
    kprintf("  CR0=%016llx  CR2=%016llx  CR3=%016llx  CR4=%016llx\n",
            (unsigned long long)read_cr0(),
            (unsigned long long)read_cr2(),
            (unsigned long long)read_cr3(),
            (unsigned long long)read_cr4());
    kprintf("  RFLAGS=%016llx\n",
            (unsigned long long)g_panic_record.rflags);
}

void NORETURN panic_at(const char *file, int line, const char *fmt, ...)
{
    char msg[PANIC_MSG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    __builtin_va_end(ap);

    disable_interrupts();

    g_panic_record.happened = 1;
    g_panic_record.rip = 0;
    g_panic_record.rsp = 0;
    g_panic_record.cr2 = read_cr2();
    g_panic_record.rflags = 0;
    strncpy(g_panic_record.message, msg, PANIC_MSG_MAX - 1);
    g_klog.panic_flag = 1;

    console_set_color(0x4, 0x0);      /* 红字黑底 */
    kprintf("\n*** FYXOS KERNEL PANIC ***\n");
    kprintf("  at %s:%d\n", file, line);
    kprintf("  %s\n", msg);
    dump_registers();
    kprintf("  system halted.\n");
    console_set_color(0x7, 0x0);

    /* 以 0xFF 通知调试器/台架：内核发生了 panic */
    arch_shutdown(0xFF);
}

void NORETURN panic(const char *fmt, ...)
{
    char msg[PANIC_MSG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    __builtin_va_end(ap);

    panic_at("<unknown>", 0, "%s", msg);
}
