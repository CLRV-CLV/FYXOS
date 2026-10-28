/*
 * FYXOS — 内核主入口 kmain()
 * ---------------------------------------------------------------------------
 * 初始化顺序是有依赖关系的，改动前请先看下面的依赖说明：
 *
 *   console_init       只开日志环形缓冲（不依赖任何设备）
 *   cpu_init           CPUID 特性探测（vmm 需要知道是否支持 NX）
 *   gdt_init           建立内核/用户段与 TSS（idt 需要段选择子）
 *   idt_init           建立 IDT（pic/pit/keyboard 需要注册中断）
 *   pic_init           重映射 8259（必须在任何设备 unmask 之前）
 *   serial/vga         注册控制台后端（均在 HHDM 窗口内，早于 vmm 也可用）
 *   pmm_init           物理帧分配器（vmm 需要它分配页表）
 *   vmm_init           重建页表（此后低端恒等映射失效）
 *   heap_init          内核堆（依赖 pmm + vmm）
 *   keyboard/pit       设备驱动（依赖 idt + pic）
 *   syscall_init       SYSCALL/SYSRET MSR（依赖 gdt + cpu）
 *   sched_init/start   抢占式调度（依赖 idt + pit + heap）
 *   storage_init       ATA → 分区 → FYXFS/VFS（在从盘装载 init 之前）
 */
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/sched.h>
#include <fyxos/syscall.h>
#include <fyxos/x86_64.h>
#include <fyxos/boot_info.h>
#include <fyxos/process.h>
#include <fyxos/storage.h>
#include <fyxos/desktop.h>
#include <fyxos/net.h>

extern u8 _kernel_end[], _kernel_phys_end[];
extern u8 _text_start[], _text_end[], _bss_start[], _bss_end[];

static struct fyx_boot_info *g_boot_info;

struct fyx_boot_info *kernel_boot_info(void)
{
    return g_boot_info;
}

static void print_banner(void)
{
    console_set_color(0xB, 0x0);
    kprintf("\n");
    kprintf("  ███████╗██╗   ██╗██╗  ██╗ ██████╗ ███████╗\n");
    kprintf("  ██╔════╝╚██╗ ██╔╝╚██╗██╔╝██╔═══██╗██╔════╝\n");
    kprintf("  █████╗   ╚████╔╝  ╚███╔╝ ██║   ██║███████╗\n");
    kprintf("  ██╔══╝    ╚██╔╝   ██╔██╗ ██║   ██║╚════██║\n");
    kprintf("  ██║        ██║   ██╔╝ ██╗╚██████╔╝███████║\n");
    kprintf("  ╚═╝        ╚═╝   ╚═╝  ╚═╝ ╚═════╝ ╚══════╝\n");
    console_set_color(0x7, 0x0);
    kprintf("\n");
    kprintf("  %s %s (%s) — 全程自研内核，无既有内核依赖\n",
            FYXOS_NAME, FYXOS_VERSION, FYXOS_CODENAME);
    kprintf("  架构 %s | 内核入口 %p | 内核结束 %p\n",
            "x86-64", _text_start, _kernel_end);
}

static void print_boot_info(struct fyx_boot_info *bi)
{
    kprintf("\n[boot] 引导信息\n");
    kprintf("  boot_info @ %p  magic=0x%x version=%u\n",
            bi, bi->magic, bi->version);
    kprintf("  引导驱动器 0x%x  可用内存 %llu KiB (%llu 字节)\n",
            bi->boot_drive,
            (unsigned long long)(bi->total_memory / 1024),
            (unsigned long long)bi->total_memory);
    kprintf("  内核映像 phys=[0x%llx, 0x%llx) virt=0x%llx\n",
            (unsigned long long)bi->kernel_phys,
            (unsigned long long)(bi->kernel_phys + bi->kernel_size),
            (unsigned long long)bi->kernel_virt);
    kprintf("  HHDM 窗口基址 0x%llx\n",
            (unsigned long long)bi->hhdm_base);

    kprintf("  E820 内存映射（%u 项）：\n", bi->e820_count);
    u32 shown = bi->e820_count < FYX_E820_MAX ? bi->e820_count : FYX_E820_MAX;
    for (u32 i = 0; i < shown; i++) {
        struct fyx_e820_entry *e = &bi->e820[i];
        if (e->length == 0)
            continue;
        const char *t = "reserved";
        switch (e->type) {
        case FYX_MEM_USABLE:   t = "usable  "; break;
        case FYX_MEM_ACPI:     t = "acpi    "; break;
        case FYX_MEM_NVS:      t = "nvs     "; break;
        case FYX_MEM_BADRAM:   t = "badram  "; break;
        default: break;
        }
        kprintf("    [%2u] 0x%016llx - 0x%016llx  %s\n",
                i, (unsigned long long)e->base,
                (unsigned long long)(e->base + e->length - 1), t);
    }
}

static void print_cpu_info(void)
{
    kprintf("\n[cpu] %s\n", g_cpu.brand[0] ? g_cpu.brand : "unknown");
    kprintf("  vendor=\"%s\" family=%u model=%u stepping=%u\n",
            g_cpu.vendor, g_cpu.family, g_cpu.model, g_cpu.stepping);
    kprintf("  特性: msr=%d tsc=%d syscall=%d nx=%d pge=%d 1G=%d sse2=%d avx=%d\n",
            g_cpu.has_msr, g_cpu.has_tsc, g_cpu.has_syscall, g_cpu.has_nx,
            g_cpu.has_pge, g_cpu.has_1gb_pages, g_cpu.has_sse2, g_cpu.has_avx);
    kprintf("  物理地址位宽 %u  虚拟地址位宽 %u\n",
            g_cpu.max_physical_bits, g_cpu.max_virtual_bits);
}

/* 演示用线程：验证抢占式调度确实在轮转 */
static volatile u64 g_demo_a, g_demo_b;
static volatile int g_demo_done;

static void demo_thread_a(void *arg)
{
    UNUSED(arg);
    for (u64 i = 0; i < 200; i++) {
        g_demo_a++;
        thread_yield();
    }
    g_demo_done |= 1;
    thread_exit(0);
}

static void demo_thread_b(void *arg)
{
    UNUSED(arg);
    for (u64 i = 0; i < 200; i++) {
        g_demo_b++;
        thread_yield();
    }
    g_demo_done |= 2;
    thread_exit(0);
}

static void run_scheduler_demo(void)
{
    kprintf("\n[sched] 启动两个抢占式内核线程做轮转演示...\n");
    struct thread *a = thread_create("demo-a", demo_thread_a, NULL, 0);
    struct thread *b = thread_create("demo-b", demo_thread_b, NULL, 0);
    if (!a || !b) {
        kprintf("  [sched] 线程创建失败\n");
        return;
    }

    u64 deadline = pit_ticks() + 2000;
    while ((g_demo_done & 3) != 3 && pit_ticks() < deadline)
        thread_yield();

    kprintf("  demo-a 计数=%llu  demo-b 计数=%llu  完成标志=0x%x\n",
            (unsigned long long)g_demo_a,
            (unsigned long long)g_demo_b, g_demo_done);
    kprintf("  -> 两个线程交替推进即证明抢占式调度与上下文切换正常\n");
}

void kmain(struct fyx_boot_info *bi)
{
    /* 最早的输出只能进日志缓冲，串口/VGA 还没注册 */
    console_init();
    g_boot_info = bi;

    cpu_init();
    gdt_init();
    idt_init();
    pic_init();

    serial_init(COM1_BASE);
    console_register_backends(CONSOLE_LOG | CONSOLE_SERIAL);

    pmm_init(bi);
    vmm_init(bi);
    heap_init();

    vga_init();
    console_register_backends(CONSOLE_LOG | CONSOLE_SERIAL | CONSOLE_VGA);

    keyboard_init();
    pit_init(100);                 /* 100 Hz -> 10 ms 一个 tick */
    syscall_init();
    sched_init();

    print_banner();
    print_cpu_info();
    print_boot_info(bi);

    {
        struct pmm_stats ps;
        pmm_stats(&ps);
        struct heap_stats hs;
        heap_stats(&hs);
        kprintf("\n[mem] 物理内存 %llu KiB 中可用 %llu KiB（%llu 帧）\n",
                (unsigned long long)(ps.total_bytes / 1024),
                (unsigned long long)(ps.free_bytes / 1024),
                (unsigned long long)ps.free_frames);
        kprintf("[mem] 内核堆 %u KiB，已用 %u KiB\n",
                (unsigned)(hs.total / 1024), (unsigned)(hs.used / 1024));
    }

    kprintf("\n[test] 运行内核自检...\n");
    int failures = kernel_selftest();
    kprintf("[test] 自检%s（失败 %d 项）\n",
            failures ? "未全部通过" : "全部通过", failures);

    /* 必须先 sched_start()：它把当前执行流（kmain）登记为 idle/boot 线程并开中断。
     * 否则下面的演示循环里 pit_ticks() 永远是 0（定时器中断进不来），
     * thread_yield() 也没有 current 线程可切换，循环无法退出。 */
    sched_start();                 /* 开启抢占 */

    run_scheduler_demo();

    /*
     * 阶段 G/H：存储就绪后，从 /bin/init 拉起第一个用户进程。
     * init 完成验收后退出，调度器会把 CPU 交回下面的桌面主循环；
     * 这样用户态测试、默认桌面与救援 Shell 可以共存。
     */
    process_init();
    if (storage_init() != 0)
        panic("storage: 无法启动持久化存储栈");
    int net_rc = net_init();
    if (net_rc != 0)
        kprintf("[net] 初始化未完成（错误 %d），仅提供离线功能\n", net_rc);
    process_start_init();

    /* 所有正常启动都以桌面为默认界面，不要求用户先理解内核 Shell。
     * 无图形设备或用户按 Q/Esc 时才进入救援 Shell。 */
    kprintf("\n[desktop] 默认进入桌面；Q/Esc 进入救援 Shell\n");
    int rc = desktop_run();
    if (rc != 0)
        kprintf("[desktop] 图形设备不可用（错误 %d），进入救援 Shell\n", rc);

    kprintf("\n[shell] 进入 FYXOS 内核 Shell，输入 help 查看命令\n");
    shell_run();

    kprintf("\n[shell] 退出，系统停机\n");
    arch_shutdown(0x00);
}
