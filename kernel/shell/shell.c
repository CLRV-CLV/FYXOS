/*
 * FYXOS — 内核 Shell
 * ---------------------------------------------------------------------------
 * 阶段 1 的命令集。所有输出都经过 console_write()，因此会同时写入
 * 内核日志环形缓冲、串口与 VGA。
 *
 * 若编译时通过 -DFYX_SHELL_SCRIPT="cmd;cmd" 指定了脚本，会先执行脚本
 * 再进入交互模式 —— fyxemu 的自动化测试依赖这个入口。
 */
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/sched.h>
#include <fyxos/syscall.h>
#include <fyxos/x86_64.h>

#define LINE_MAX    160
#define MAX_ARGS    16

static bool g_running = true;
static char g_line[LINE_MAX];
static int  g_pos;
static u32  g_cmd_count;

/* ============================== 行编辑 =================================== */
static void line_reset(void)
{
    g_pos = 0;
    memset(g_line, 0, sizeof(g_line));
}

/* 返回 true 表示一行已结束 */
static bool line_put(char c)
{
    if (c == '\b' || c == 0x7F) {
        if (g_pos > 0) {
            g_pos--;
            g_line[g_pos] = '\0';
            console_putc('\b');
            console_putc(' ');
            console_putc('\b');
        }
        return false;
    }
    if (c == '\r' || c == '\n') {
        console_putc('\n');
        return true;
    }
    if (g_pos < (int)sizeof(g_line) - 1 && isprint(c)) {
        g_line[g_pos++] = c;
        console_putc(c);
    }
    return false;
}

/* ============================== 命令表 ================================== */
typedef int (*cmd_fn)(int argc, char **argv);

struct command {
    const char *name;
    const char *usage;
    const char *desc;
    cmd_fn      fn;
};

static int cmd_help(int argc, char **argv);
static int cmd_uname(int argc, char **argv);
static int cmd_mem(int argc, char **argv);
static int cmd_threads(int argc, char **argv);
static int cmd_uptime(int argc, char **argv);
static int cmd_echo(int argc, char **argv);
static int cmd_test(int argc, char **argv);
static int cmd_alloc(int argc, char **argv);
static int cmd_log(int argc, char **argv);
static int cmd_yield(int argc, char **argv);
static int cmd_sleep(int argc, char **argv);
static int cmd_fault(int argc, char **argv);
static int cmd_clear(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_shutdown(int argc, char **argv);
static int cmd_exit(int argc, char **argv);

static const struct command *const g_commands[] = {
    &(const struct command){ "help",     "help [命令]",       "显示命令列表或某个命令的用法", cmd_help },
    &(const struct command){ "uname",    "uname",             "显示内核与 CPU 信息",          cmd_uname },
    &(const struct command){ "mem",      "mem",               "显示物理内存与堆统计",          cmd_mem },
    &(const struct command){ "ps",       "ps",                "列出所有内核线程",              cmd_threads },
    &(const struct command){ "uptime",   "uptime",            "显示系统启动以来的 tick 数",    cmd_uptime },
    &(const struct command){ "echo",     "echo <文本>",       "原样回显",                      cmd_echo },
    &(const struct command){ "test",     "test",              "运行内核自检",                  cmd_test },
    &(const struct command){ "alloc",    "alloc <字节数>",    "分配并显示内核堆内存",          cmd_alloc },
    &(const struct command){ "log",      "log",               "重放内核日志缓冲",              cmd_log },
    &(const struct command){ "yield",    "yield",             "主动让出 CPU",                  cmd_yield },
    &(const struct command){ "sleep",    "sleep <毫秒>",      "睡眠若干毫秒",                  cmd_sleep },
    &(const struct command){ "fault",    "fault",             "故意触发一次异常（测试路径）",  cmd_fault },
    &(const struct command){ "clear",    "clear",             "清屏",                          cmd_clear },
    &(const struct command){ "reboot",   "reboot",            "重启",                          cmd_reboot },
    &(const struct command){ "shutdown", "shutdown",          "关机",                          cmd_shutdown },
    &(const struct command){ "exit",     "exit",              "退出 Shell 并停机",             cmd_exit },
};

static const struct command *find_command(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_commands); i++) {
        if (g_commands[i] && strcmp(g_commands[i]->name, name) == 0)
            return g_commands[i];
    }
    return NULL;
}

/* ============================== 命令实现 ================================ */
static int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        const struct command *c = find_command(argv[1]);
        if (!c) {
            kprintf("未知命令: %s\n", argv[1]);
            return 1;
        }
        kprintf("用法: %s\n  %s\n", c->usage, c->desc);
        return 0;
    }
    kprintf("\n可用命令：\n");
    for (size_t i = 0; i < ARRAY_SIZE(g_commands); i++) {
        if (!g_commands[i])
            continue;
        kprintf("  %-10s %-22s %s\n", g_commands[i]->name,
                g_commands[i]->usage, g_commands[i]->desc);
    }
    kprintf("\n");
    return 0;
}

static int cmd_uname(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    kprintf("%s %s (%s) x86-64\n", FYXOS_NAME, FYXOS_VERSION, FYXOS_CODENAME);
    kprintf("CPU: %s\n", g_cpu.brand[0] ? g_cpu.brand : "unknown");
    kprintf("特性: syscall=%d nx=%d tsc=%d apic=%d\n",
            g_cpu.has_syscall, g_cpu.has_nx, g_cpu.has_tsc, g_cpu.has_apic);
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    struct pmm_stats ps;
    pmm_stats(&ps);
    struct heap_stats hs;
    heap_stats(&hs);

    kprintf("物理内存\n");
    kprintf("  总计     %8llu KiB\n", (unsigned long long)(ps.total_bytes / 1024));
    kprintf("  可用     %8llu KiB (%llu 帧)\n",
            (unsigned long long)(ps.free_bytes / 1024),
            (unsigned long long)ps.free_frames);
    kprintf("  已用     %8llu KiB\n", (unsigned long long)(ps.used_bytes / 1024));
    kprintf("  保留     %8llu 帧\n", (unsigned long long)ps.reserved_frames);
    kprintf("内核堆\n");
    kprintf("  总容量   %8u KiB\n", (unsigned)(hs.total / 1024));
    kprintf("  已用     %8u KiB\n", (unsigned)(hs.used / 1024));
    kprintf("  峰值     %8u KiB\n", (unsigned)(hs.peak_used / 1024));
    kprintf("  分配/释放 %llu / %llu  失败 %llu\n",
            (unsigned long long)hs.alloc_count,
            (unsigned long long)hs.free_count,
            (unsigned long long)hs.fail_count);
    return 0;
}

static int cmd_threads(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    static const char *const state_names[] = {
        "unused", "embryo", "runnable", "running",
        "sleep",  "block",  "zombie"
    };
    struct sched_stats ss;
    sched_stats(&ss);

    kprintf("  %-5s %-16s %-10s %-10s %-8s\n",
            "TID", "名称", "状态", "累计tick", "切换数");
    for (u64 tid = 0; tid <= MAX_THREADS; tid++) {
        struct thread *t = thread_by_tid(tid);
        if (!t)
            continue;
        const char *st = (t->state < 7) ? state_names[t->state] : "?";
        /*
         * idle 线程的状态恒为 RUNNING（它不入运行队列，只在队列空时被选中），
         * 直接照状态打印会让 ps 里同时出现两条 running。这里改成按"是否真的
         * 在占用 CPU"来显示。
         */
        if (current_thread() == t)
            st = "running";
        else if (t->state == 3 /* THREAD_RUNNING */)
            st = "idle";
        kprintf("  %-5llu %-16s %-10s %-10llu %-8llu%s\n",
                (unsigned long long)t->tid, t->name, st,
                (unsigned long long)t->total_ticks,
                (unsigned long long)t->switches,
                (current_thread() == t) ? "  <- 当前" : "");
    }
    kprintf("  调度器: tick=%llu 切换=%llu 抢占=%llu 主动让出=%llu\n",
            (unsigned long long)ss.ticks, (unsigned long long)ss.switches,
            (unsigned long long)ss.preemptions, (unsigned long long)ss.yields);
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    u64 ticks = pit_ticks();
    u32 freq = pit_frequency();
    kprintf("  已运行 %llu 个 tick（PIT %u Hz，约 %llu 毫秒）\n",
            (unsigned long long)ticks, freq,
            (unsigned long long)(freq ? (ticks * 1000 / freq) : 0));
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        kprintf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    kprintf("\n");
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    int failures = kernel_selftest();
    kprintf("自检结果: %s（失败 %d 项）\n",
            failures ? "存在失败" : "全部通过", failures);
    return failures;
}

static int cmd_alloc(int argc, char **argv)
{
    size_t n = 1024;
    if (argc > 1)
        n = (size_t)strtoul(argv[1], NULL, 0);
    if (n == 0 || n > (16u << 20)) {
        kprintf("  字节数必须在 1 .. 16MiB 之间\n");
        return 1;
    }
    char *p = kmalloc(n);
    if (!p) {
        kprintf("  分配失败\n");
        return 1;
    }
    memset(p, 0xAB, n);
    kprintf("  分配到 %p，写入 %u 字节，首字节=0x%02x\n",
            p, (unsigned)n, (unsigned)(u8)p[0]);
    kfree(p);
    kprintf("  已释放\n");
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    /* 从环形缓冲里取出最后 min(head, size) 个字节 */
    u32 head = g_klog.head;
    u32 count = (head < FYX_LOG_SIZE) ? head : FYX_LOG_SIZE;
    u32 start = (head < FYX_LOG_SIZE) ? 0 : (head % FYX_LOG_SIZE);
    kprintf("--- 内核日志（共 %u 字节，已写入 %u 字节）---\n",
            count, head);
    for (u32 i = 0; i < count; i++) {
        char c = g_klog.buf[(start + i) % FYX_LOG_SIZE];
        console_putc(c);
    }
    kprintf("\n--- 日志结束 ---\n");
    return 0;
}

static int cmd_yield(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    thread_yield();
    kprintf("  已让出 CPU 并恢复执行\n");
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    u64 ms = (argc > 1) ? strtoul(argv[1], NULL, 0) : 100;
    u64 before = pit_ticks();
    thread_sleep_ms(ms);
    kprintf("  睡眠完成（%llu -> %llu tick）\n",
            (unsigned long long)before, (unsigned long long)pit_ticks());
    return 0;
}

static int cmd_fault(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    kprintf("  即将向 0xDEADBEEF 写入，触发缺页异常...\n");
    volatile u64 *bad = (volatile u64 *)0xDEADBEEF;
    *bad = 0x1234;
    kprintf("  未触发异常（这不应该发生）\n");
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    vga_clear();
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    kprintf("  重启...\n");
    arch_reboot();
    return 0;
}

static int cmd_shutdown(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    kprintf("  关机。\n");
    g_running = false;
    return 0;
}

static int cmd_exit(int argc, char **argv)
{
    return cmd_shutdown(argc, argv);
}

/* ============================== 命令执行 ================================ */
static int execute_line(char *line)
{
    if (!line)
        return 0;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0')
        return 0;

    char *argv[MAX_ARGS];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t", &save);
         tok && argc < MAX_ARGS;
         tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0)
        return 0;

    const struct command *c = find_command(argv[0]);
    if (!c) {
        kprintf("%s: 未知命令，输入 help 查看可用命令\n", argv[0]);
        return 127;
    }
    g_cmd_count++;
    return c->fn(argc, argv);
}

/* 执行用分号分隔的脚本（自动化测试用） */
static void run_script(const char *script)
{
    if (!script || !*script)
        return;
    char buf[LINE_MAX];
    size_t n = strlen(script);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, script, n);
    buf[n] = '\0';

    char *save = NULL;
    for (char *cmd = strtok_r(buf, ";", &save);
         cmd;
         cmd = strtok_r(NULL, ";", &save)) {
        kprintf("fyxos> %s\n", cmd);
        execute_line(cmd);
    }
}

/* ================================ 主循环 ================================ */
void shell_run(void)
{
#ifdef FYX_SHELL_SCRIPT
    run_script(FYX_SHELL_SCRIPT);
#endif

    while (g_running) {
        kprintf("fyxos> ");
        line_reset();

        for (;;) {
            char c = keyboard_getchar();
            if (line_put(c))
                break;
        }

        if (execute_line(g_line) == 127)
            continue;
    }
    kprintf("共执行 %u 条命令\n", g_cmd_count);
}
