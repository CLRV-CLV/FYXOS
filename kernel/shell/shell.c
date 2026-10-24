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
#include <fyxos/vfs.h>
#include <fyxos/storage.h>
#include <fyxos/block.h>
#include <fyxos/net.h>
#include <fyxos/desktop.h>
#include <fyxos/fyxcc.h>
#include <fyxos/fyxgit.h>
#include <fyxos/ide.h>
#include <fyxos/process.h>

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
static int cmd_ls(int argc, char **argv);
static int cmd_cat(int argc, char **argv);
static int cmd_write(int argc, char **argv);
static int cmd_mkdir(int argc, char **argv);
static int cmd_rm(int argc, char **argv);
static int cmd_cp(int argc, char **argv);
static int cmd_stat(int argc, char **argv);
static int cmd_df(int argc, char **argv);
static int cmd_fsck(int argc, char **argv);
static int cmd_sync(int argc, char **argv);
static int cmd_mount(int argc, char **argv);
static int cmd_unmount(int argc, char **argv);
static int cmd_snapshot(int argc, char **argv);
static int cmd_rollback(int argc, char **argv);
static int cmd_ifconfig(int argc, char **argv);
static int cmd_ping(int argc, char **argv);
static int cmd_ide(int argc, char **argv);
static int cmd_desktop(int argc, char **argv);
static int cmd_fyxcc(int argc, char **argv);
static int cmd_git(int argc, char **argv);
static int cmd_run(int argc, char **argv);

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
    &(const struct command){ "ls",       "ls [路径]",         "列出目录（dir 为兼容别名）",     cmd_ls },
    &(const struct command){ "dir",      "dir [路径]",        "ls 的跨系统命令别名",            cmd_ls },
    &(const struct command){ "cat",      "cat <文件>",        "显示文件内容（type 为别名）",    cmd_cat },
    &(const struct command){ "type",     "type <文件>",       "cat 的跨系统命令别名",           cmd_cat },
    &(const struct command){ "write",    "write <文件> <文本>", "原子替换文件内容",             cmd_write },
    &(const struct command){ "mkdir",    "mkdir <路径>",      "创建目录（md 为别名）",          cmd_mkdir },
    &(const struct command){ "md",       "md <路径>",         "mkdir 的跨系统命令别名",         cmd_mkdir },
    &(const struct command){ "rm",       "rm <路径>",         "删除空目录或文件（del 为别名）", cmd_rm },
    &(const struct command){ "del",      "del <路径>",        "rm 的跨系统命令别名",            cmd_rm },
    &(const struct command){ "cp",       "cp <源> <目标>",    "复制文件",                       cmd_cp },
    &(const struct command){ "stat",     "stat <路径>",       "显示节点、大小与 CRC32",         cmd_stat },
    &(const struct command){ "df",       "df",                "显示 FYXFS 空间与事务状态",      cmd_df },
    &(const struct command){ "fsck",     "fsck",              "校验根、快照和文件数据",         cmd_fsck },
    &(const struct command){ "sync",     "sync",              "把 ATA 写缓存刷入介质",          cmd_sync },
    &(const struct command){ "mount",    "mount",             "重新挂载系统分区",               cmd_mount },
    &(const struct command){ "unmount",  "unmount",           "卸载系统分区",                   cmd_unmount },
    &(const struct command){ "snapshot", "snapshot [create 名]", "列出或创建 CoW 快照",         cmd_snapshot },
    &(const struct command){ "rollback", "rollback <id> [all]", "回滚；默认保留 /home",          cmd_rollback },
    &(const struct command){ "ifconfig", "ifconfig",           "显示 IPv4、MAC 与网络统计",      cmd_ifconfig },
    &(const struct command){ "ping",     "ping [IPv4] [毫秒]",  "向网关或指定 IPv4 发送 ICMP echo", cmd_ping },
    &(const struct command){ "desktop",  "desktop [run|next|theme|info]", "启动/控制软件合成图形桌面", cmd_desktop },
    &(const struct command){ "ide",      "ide <source.c>",        "图形编辑器：保存/编译/运行",   cmd_ide },
    &(const struct command){ "fyxcc",    "fyxcc <源文件> <ELF>", "内置 C 子集原生编译器",       cmd_fyxcc },
    &(const struct command){ "cc",       "cc <源文件> <ELF>",   "fyxcc 的跨系统命令别名",       cmd_fyxcc },
    &(const struct command){ "git",      "git <子命令> ...",     "FYXGit v1：暂存/提交/diff/恢复", cmd_git },
    &(const struct command){ "run",      "run <ELF>",           "从磁盘装载并等待用户程序",       cmd_run },
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

/* ============================== 网络 ==================================== */
static void shell_print_ip(u32 ip)
{
    kprintf("%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF);
}

static bool shell_parse_ip(const char *text, u32 *out)
{
    if (!text || !out)
        return false;
    u32 value = 0;
    for (u32 part = 0; part < 4; part++) {
        char *end = NULL;
        unsigned long n = strtoul(text, &end, 10);
        if (end == text || n > 255 || (part < 3 && *end != '.') ||
            (part == 3 && *end != '\0'))
            return false;
        value = (value << 8) | (u32)n;
        text = end + (part < 3 ? 1 : 0);
    }
    *out = value;
    return true;
}

static int cmd_ifconfig(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    if (!net_present()) {
        kprintf("eth0: 离线（未发现 RTL8139）\n");
        return 0;
    }
    net_poll();                         /* 也覆盖空环/待收包两种 poll 路径 */
    struct net_info info;
    struct net_stats stats;
    net_get_info(&info);
    net_get_stats(&stats);
    kprintf("eth0: %s\n", info.ready ? "UP" : "DOWN");
    kprintf("  MAC  %02x:%02x:%02x:%02x:%02x:%02x\n",
            info.mac[0], info.mac[1], info.mac[2], info.mac[3],
            info.mac[4], info.mac[5]);
    kprintf("  IPv4 "); shell_print_ip(info.ip);
    kprintf("  掩码 "); shell_print_ip(info.netmask);
    kprintf("  网关 "); shell_print_ip(info.gateway); kprintf("\n");
    kprintf("  网关 MAC %02x:%02x:%02x:%02x:%02x:%02x (%s)\n",
            info.gateway_mac[0], info.gateway_mac[1], info.gateway_mac[2],
            info.gateway_mac[3], info.gateway_mac[4], info.gateway_mac[5],
            info.gateway_mac_valid ? "已解析" : "未解析");
    kprintf("  RX=%llu TX=%llu 错误=%llu ARP请求=%llu ARP回应=%llu "
            "ICMP回应=%llu UDP收/发=%llu/%llu\n",
            (unsigned long long)stats.rx_packets,
            (unsigned long long)stats.tx_packets,
            (unsigned long long)stats.rx_errors,
            (unsigned long long)stats.arp_requests,
            (unsigned long long)stats.arp_replies,
            (unsigned long long)stats.icmp_echo_replies,
            (unsigned long long)stats.udp_received,
            (unsigned long long)stats.udp_sent);
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    if (!net_ready()) {
        kprintf("ping: 网络尚未就绪\n");
        return 1;
    }
    struct net_info info;
    net_get_info(&info);
    u32 destination = info.gateway;
    if (argc > 1 && !shell_parse_ip(argv[1], &destination)) {
        kprintf("ping: 需要点分十进制 IPv4 地址\n");
        return 1;
    }
    u32 timeout = argc > 2 ? (u32)strtoul(argv[2], NULL, 0) : 100;
    if (!timeout)
        timeout = 100;
    u32 rtt = 0;
    int rc = net_ping(destination, timeout, &rtt);
    if (rc != 0) {
        kprintf("ping: ");
        shell_print_ip(destination);
        kprintf(" 失败（错误 %d）\n", rc);
        return 1;
    }
    kprintf("ping ");
    shell_print_ip(destination);
    kprintf(": ICMP echo 回复，time=%u ms\n", rtt);
    return 0;
}

static int cmd_ide(int argc, char **argv)
{
    if (argc != 2) {
        kprintf("用法: ide <source.c>\n");
        return 1;
    }
    int rc = ide_open(argv[1]);
    if (rc != 0) {
        kprintf("ide: 无法打开文件（%d）\n", rc);
        return 1;
    }
    rc = ide_run();
    if (rc != 0) {
        kprintf("ide: 图形编辑器不可用（%d）\n", rc);
        return 1;
    }
    return 0;
}

static int cmd_desktop(int argc, char **argv)
{
    int rc;
    if (argc == 1)
        rc = desktop_start();
    else if (strcmp(argv[1], "run") == 0)
        rc = desktop_run();
    else if (strcmp(argv[1], "next") == 0)
        rc = desktop_next_window();
    else if (strcmp(argv[1], "theme") == 0)
        rc = desktop_toggle_theme();
    else if (strcmp(argv[1], "info") == 0) {
        desktop_print_info();
        return 0;
    } else {
        kprintf("用法: desktop [run|next|theme|info]\n");
        return 1;
    }
    if (rc != 0) {
        kprintf("desktop: graphics unavailable/init failed (error %d)\n", rc);
        return 1;
    }
    return 0;
}

static int cmd_fyxcc(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("用法: %s <源文件> <输出ELF>\n", argv[0]);
        return 1;
    }
    struct fyxcc_diagnostic diagnostic;
    int rc = fyxcc_compile_file(argv[1], argv[2], &diagnostic);
    if (rc != 0) {
        kprintf("%s: 编译失败（错误 %d）", argv[0], rc);
        if (diagnostic.message[0])
            kprintf("：%s（偏移 %u）", diagnostic.message,
                    (unsigned)diagnostic.offset);
        kprintf("\n");
        return 1;
    }
    struct vfs_stat st;
    if (vfs_stat(argv[2], &st) != 0)
        return 1;
    kprintf("[cc] %s -> %s（%llu 字节，x86_64 ET_EXEC）\n",
            argv[1], argv[2], (unsigned long long)st.size);
    return 0;
}

static int cmd_git(int argc, char **argv)
{
    return fyxgit_command(argc, argv);
}

static int cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("用法: run <ELF>\n");
        return 1;
    }
    int status = 0;
    int rc = process_run_wait(argv[1], &status);
    if (rc != 0) {
        kprintf("run: 无法启动 %s（错误 %d）\n", argv[1], rc);
        return 1;
    }
    kprintf("[run] %s 退出码=%d\n", argv[1], status);
    return status;
}

/* ========================== 文件与系统还原 ============================== */
static int cmd_ls(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    struct vfs_stat st;
    int rc = vfs_stat(path, &st);
    if (rc != 0) {
        kprintf("ls: %s: 错误 %d\n", path, rc);
        return 1;
    }
    if (st.type != FYXFS_NODE_DIR) {
        kprintf("%s  %llu 字节\n", path, (unsigned long long)st.size);
        return 0;
    }
    kprintf("%s：\n", path);
    for (u32 i = 0;; i++) {
        struct fyxfs_node_info info;
        rc = vfs_getdents(path, i, &info);
        if (rc == -FYX_ENOENT)
            break;
        if (rc != 0)
            return 1;
        kprintf("  %-24s %-4s %8llu 字节  node=%u\n", info.name,
                info.type == FYXFS_NODE_DIR ? "dir" : "file",
                (unsigned long long)info.size, info.node);
    }
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("用法: cat <文件>\n");
        return 1;
    }
    struct fyx_file *file;
    int rc = vfs_open(argv[1], VFS_O_RDONLY, &file);
    if (rc != 0) {
        kprintf("cat: %s: 错误 %d\n", argv[1], rc);
        return 1;
    }
    char buf[512];
    long n;
    char last = 0;
    while ((n = vfs_file_read(file, buf, sizeof(buf))) > 0) {
        console_write(buf, (size_t)n);
        last = buf[n - 1];
    }
    vfs_file_close(file);
    if (last && last != '\n')
        console_putc('\n');
    return n < 0 ? 1 : 0;
}

static int cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("用法: write <文件> <文本>\n");
        return 1;
    }
    char buf[1024];
    size_t used = 0;
    for (int i = 2; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (used + n + 2 > sizeof(buf)) {
            kprintf("write: 文本过长\n");
            return 1;
        }
        if (i > 2)
            buf[used++] = ' ';
        memcpy(buf + used, argv[i], n);
        used += n;
    }
    buf[used++] = '\n';

    /* fyxemu 可用这个普通日志标记在随后的任意 ATA 写点模拟断电。 */
    kprintf("[storage-write] %s\n", argv[1]);
    int rc = vfs_write_file(argv[1], buf, used);
    if (rc == 0)
        rc = vfs_sync();
    if (rc != 0) {
        kprintf("write: 错误 %d\n", rc);
        return 1;
    }
    kprintf("已原子写入 %s（%u 字节）\n", argv[1], (unsigned)used);
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    int rc = vfs_mkdir(argv[1]);
    if (rc != 0)
        kprintf("mkdir: 错误 %d\n", rc);
    return rc != 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    int rc = vfs_unlink(argv[1]);
    if (rc != 0)
        kprintf("rm: 错误 %d\n", rc);
    return rc != 0;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("用法: cp <源> <目标>\n");
        return 1;
    }
    void *data;
    size_t size;
    int rc = vfs_read_file(argv[1], &data, &size);
    if (rc == 0) {
        rc = vfs_write_file(argv[2], data, size);
        kfree(data);
    }
    if (rc != 0)
        kprintf("cp: 错误 %d\n", rc);
    return rc != 0;
}

static int cmd_stat(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    struct vfs_stat st;
    int rc = vfs_stat(argv[1], &st);
    if (rc != 0) {
        kprintf("stat: 错误 %d\n", rc);
        return 1;
    }
    kprintf("%s: node=%u type=%s size=%llu crc32=0x%08x\n", argv[1],
            st.node, st.type == FYXFS_NODE_DIR ? "dir" : "file",
            (unsigned long long)st.size, st.crc32);
    return 0;
}

static int cmd_df(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    struct fyxfs_stats st;
    vfs_stats(&st);
    struct block_stats io;
    block_get_stats(&io);
    kprintf("FYXFS generation=%llu root=%u\n",
            (unsigned long long)st.generation, st.active_root);
    kprintf("  块区: 追加分配 %u / %u（每块 4 KiB）\n",
            st.next_block, st.total_blocks);
    kprintf("  节点 %u，快照 %u；ATA 读/写 %llu/%llu 扇区，flush %llu，错误 %llu\n",
            st.nodes, st.snapshots,
            (unsigned long long)io.read_sectors,
            (unsigned long long)io.write_sectors,
            (unsigned long long)io.flushes,
            (unsigned long long)io.io_errors);
    return 0;
}

static int cmd_fsck(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    u32 errors = 0;
    int rc = vfs_check(false, &errors);
    return rc != 0 || errors != 0;
}

static int cmd_sync(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    int rc = vfs_sync();
    if (rc == 0)
        kprintf("已同步到 ATA 介质\n");
    return rc != 0;
}

static int cmd_mount(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    if (vfs_mounted()) {
        kprintf("系统分区已经挂载\n");
        return 0;
    }
    int rc = storage_mount();
    if (rc != 0)
        kprintf("mount: 错误 %d\n", rc);
    return rc != 0;
}

static int cmd_unmount(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    int rc = storage_unmount();
    if (rc != 0)
        kprintf("unmount: 错误 %d（可能有打开的文件）\n", rc);
    return rc != 0;
}

static int cmd_snapshot(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "create") == 0) {
        const char *name = argc > 2 ? argv[2] : "manual";
        u64 id;
        int rc = vfs_snapshot_create(name, &id);
        if (rc == 0)
            kprintf("快照 #%llu 已创建\n", (unsigned long long)id);
        else
            kprintf("snapshot: 错误 %d\n", rc);
        return rc != 0;
    }
    kprintf("ID   generation   root       名称\n");
    for (u32 i = 0;; i++) {
        struct fyxfs_snapshot_info info;
        int rc = vfs_snapshot_get(i, &info);
        if (rc == -FYX_ENOENT)
            break;
        if (rc != 0)
            return 1;
        kprintf("#%-3llu %-12llu %-10u %s\n",
                (unsigned long long)info.id,
                (unsigned long long)info.generation,
                info.root_block, info.name);
    }
    return 0;
}

static int cmd_rollback(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("用法: rollback <id> [all]；默认只回滚系统并保留 /home\n");
        return 1;
    }
    u64 id = strtoul(argv[1], NULL, 0);
    bool all = argc > 2 && strcmp(argv[2], "all") == 0;
    int rc = vfs_snapshot_rollback(id, !all);
    if (rc == 0)
        rc = vfs_sync();
    if (rc != 0)
        kprintf("rollback: 错误 %d\n", rc);
    return rc != 0;
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
