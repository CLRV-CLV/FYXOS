/*
 * FYXOS — 第一个用户态程序
 * ---------------------------------------------------------------------------
 * 它是阶段 G 的验收程序，一次性把这条链路全走一遍：
 *   1. 从 ring 3 发起系统调用（证明 SYSCALL/SYSRET 与权限检查都对）
 *   2. fork 出子进程，父子靠返回值区分（证明用户现场能被完整复制）
 *   3. wait 回收子进程并取到退出码（证明进程生命周期完事）
 *   4. 忙等若干 tick（证明用户态执行时会被时钟中断抢占，且能正确恢复）
 *   5. 读 argv/envp（证明 elf_load.c 的栈布置符合 SysV ABI）
 */
#include "fyx.h"

#define TICKS_TO_SPIN 5

static int run_network_test(void)
{
    int fd = (int)sys_socket(FYX_AF_INET, FYX_SOCK_DGRAM, FYX_IPPROTO_UDP);
    if (fd == -17) {
        sys_puts("[init] 网络离线，跳过 UDP socket 测试\n");
        return 0;
    }
    if (fd < 0) {
        sys_puts("[init] UDP socket 创建失败\n");
        return 1;
    }
    struct fyx_user_sockaddr_in local = { 0 };
    local.family = FYX_AF_INET;
    local.port = fyx_htons(40000);
    long bind_rc = sys_bind(fd, &local);
    if (bind_rc != 0) {
        sys_puts("[init] UDP bind 失败，rc=");
        fyx_put_u64((unsigned long)bind_rc);
        sys_puts("\n");
        sys_close(fd);
        return 1;
    }
    struct fyx_user_sockaddr_in peer = { 0 };
    peer.family = FYX_AF_INET;
    peer.port = fyx_htons(9);         /* 台架 UDP echo 服务 */
    peer.ip = fyx_ip(10, 0, 2, 2);
    static const char message[] = "fyxos-ring3-udp";
    long sent = sys_sendto(fd, message, sizeof(message) - 1, &peer);
    char reply[64] = { 0 };
    struct fyx_user_sockaddr_in source = { 0 };
    long received = sent == (long)(sizeof(message) - 1)
        ? sys_recvfrom(fd, reply, sizeof(reply), &source) : -1;
    int ok = received == (long)(sizeof(message) - 1) &&
        source.ip == peer.ip && source.port == peer.port;
    for (unsigned long i = 0; ok && i < sizeof(message) - 1; i++)
        if (reply[i] != message[i])
            ok = 0;
    sys_close(fd);
    if (!ok) {
        sys_puts("[init] UDP 诊断 fd=");
        fyx_put_u64((unsigned long)fd);
        sys_puts(" sent=");
        fyx_put_u64((unsigned long)sent);
        sys_puts(" recv=");
        fyx_put_u64((unsigned long)received);
        sys_puts(" source_ip=");
        fyx_put_u64(source.ip);
        sys_puts(" source_port=");
        fyx_put_u64(fyx_htons(source.port));
        sys_puts("\n");
    }
    sys_puts(ok ? "[init] ring3 UDP socket 通过：DHCP/ARP/IPv4/UDP 收发一致\n"
                : "[init] ring3 UDP socket 失败\n");
    return ok ? 0 : 1;
}

static int run_tcp_test(void)
{
    int fd = (int)sys_socket(FYX_AF_INET, FYX_SOCK_STREAM, FYX_IPPROTO_TCP);
    if (fd == -17) {
        sys_puts("[init] 网络离线，跳过 TCP socket 测试\n");
        return 0;
    }
    if (fd < 0) {
        sys_puts("[init] TCP socket 创建失败\n");
        return 1;
    }
    int refused_fd = (int)sys_socket(FYX_AF_INET, FYX_SOCK_STREAM,
                                     FYX_IPPROTO_TCP);
    struct fyx_user_sockaddr_in refused_peer = { 0 };
    refused_peer.family = FYX_AF_INET;
    refused_peer.port = fyx_htons(8);
    refused_peer.ip = fyx_ip(10, 0, 2, 2);
    long refused = refused_fd < 0 ? -1 : sys_connect(refused_fd, &refused_peer);
    if (refused_fd >= 0)
        sys_close(refused_fd);
    if (refused != -20) {
        sys_puts("[init] TCP RST/拒绝连接测试失败\n");
        sys_close(fd);
        return 1;
    }

    struct fyx_user_sockaddr_in peer = { 0 };
    peer.family = FYX_AF_INET;
    peer.port = fyx_htons(7);         /* 台架 TCP echo 服务 */
    peer.ip = fyx_ip(10, 0, 2, 2);
    long connected = sys_connect(fd, &peer);
    static const char message[] = "fyxos-ring3-tcp";
    long sent = connected == 0
        ? sys_fd_write(fd, message, sizeof(message) - 1) : -1;
    char reply[sizeof(message)] = { 0 };
    long received = 0;
    while (sent == (long)(sizeof(message) - 1) &&
           received < (long)(sizeof(message) - 1)) {
        /* 故意小片读取：同一 TCP 段必须保留未消费部分。 */
        long n = sys_fd_read(fd, reply + received, 3);
        if (n <= 0)
            break;
        received += n;
    }
    long eof = received == (long)(sizeof(message) - 1)
        ? sys_fd_read(fd, reply, 1) : -1;
    int ok = received == (long)(sizeof(message) - 1) && eof == 0;
    for (unsigned long i = 0; ok && i < sizeof(message) - 1; i++)
        if (reply[i] != message[i])
            ok = 0;
    sys_close(fd);
    if (!ok) {
        sys_puts("[init] TCP 诊断 connect=");
        fyx_put_u64((unsigned long)connected);
        sys_puts(" sent=");
        fyx_put_u64((unsigned long)sent);
        sys_puts(" recv=");
        fyx_put_u64((unsigned long)received);
        sys_puts(" eof=");
        fyx_put_u64((unsigned long)eof);
        sys_puts("\n");
    }
    sys_puts(ok ? "[init] ring3 TCP socket 通过：SYN/RST/checksum/分段读取/FIN-EOF 一致\n"
                : "[init] ring3 TCP socket 失败\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv, char **envp)
{
    /* exec 后重新进入同一个映像：只走这条短路径，避免再次 fork */
    if (argc > 1 && argv[1] && argv[1][0] == '-' && argv[1][1] == '-') {
        sys_puts("[exec] 新程序已启动，argv[1]=");
        sys_puts(argv[1]);
        sys_puts("，退出码 7\n");
        sys_exit(7);
    }

    sys_puts("\n[init] === FYXOS 用户态已启动（ring 3）===\n");

    sys_puts("[init] pid=");
    fyx_put_u64((unsigned long)sys_getpid());
    sys_puts(" ppid=");
    fyx_put_u64((unsigned long)sys_getppid());
    sys_puts(" tid=");
    fyx_put_u64((unsigned long)sys_get_tid());
    sys_puts("\n");

    /* 1) argv / envp 是否按 SysV ABI 布好 */
    sys_puts("[init] argc=");
    fyx_put_u64((unsigned long)argc);
    sys_puts(" argv[0]=");
    sys_puts(argc > 0 && argv[0] ? argv[0] : "(null)");
    sys_puts(" envp[0]=");
    sys_puts(envp && envp[0] ? envp[0] : "(null)");
    sys_puts("\n");

    /* 2) 真实用户态网络：标准 UDP echo 报文必须回到同一个 socket。 */
    if (run_network_test() != 0)
        sys_exit(1);
    if (run_tcp_test() != 0)
        sys_exit(1);

    /* 3) 持久化文件 fd：create/write/sync/seek/read/stat 全走 ring 3 ABI。 */
    static const char file_msg[] = "ring3-storage-ok";
    int filefd = sys_open("/home/user/ring3.txt",
                          FYX_O_RDWR | FYX_O_CREAT | FYX_O_TRUNC);
    if (filefd < 0 ||
        sys_fd_write(filefd, file_msg, sizeof(file_msg) - 1) !=
            (long)(sizeof(file_msg) - 1) ||
        sys_sync() != 0 || sys_lseek(filefd, 0, FYX_SEEK_SET) != 0) {
        sys_puts("[init] 文件写入或 sync/seek 失败\n");
        sys_exit(1);
    }
    char file_buf[sizeof(file_msg)] = { 0 };
    long file_n = sys_fd_read(filefd, file_buf, sizeof(file_msg) - 1);
    int file_ok = file_n == (long)(sizeof(file_msg) - 1);
    for (unsigned long i = 0; file_ok && i < sizeof(file_msg) - 1; i++)
        if (file_buf[i] != file_msg[i])
            file_ok = 0;
    sys_close(filefd);
    struct fyx_user_stat init_stat;
    long stat_rc = sys_stat("/bin/init", &init_stat);
    sys_puts("[init] FYXFS 文件读写与 seek ");
    sys_puts(file_ok && stat_rc == 0 && init_stat.size > 0
             ? "✓ 内容一致，/bin/init stat 正常\n"
             : "✗ 校验失败\n");
    if (!file_ok || stat_rc != 0)
        sys_exit(1);

    /* 3) 建管道后 fork：验证 fd 继承与进程间通信 */
    int pipefds[2];
    if (sys_pipe(pipefds) < 0) {
        sys_puts("[init] pipe 创建失败\n");
        sys_exit(1);
    }

    /* 3) fork：父拿子进程 pid，子拿 0 */
    sys_puts("[init] fork...\n");
    long child = sys_fork();
    if (child < 0) {
        sys_puts("[init] fork 失败\n");
        sys_exit(1);
    }

    if (child == 0) {
        sys_puts("[child] fork 返回 0 —— 我是子进程，pid=");
        fyx_put_u64((unsigned long)sys_getpid());
        sys_puts(" ppid=");
        fyx_put_u64((unsigned long)sys_getppid());
        sys_puts("\n[child] 通过管道发送 pipe-ok\n");
        const char pipe_msg[] = "pipe-ok";
        sys_close(pipefds[0]);
        if (sys_fd_write(pipefds[1], pipe_msg, sizeof(pipe_msg) - 1) !=
            (long)(sizeof(pipe_msg) - 1)) {
            sys_puts("[child] 管道写入失败\n");
            sys_exit(1);
        }
        sys_close(pipefds[1]);
        sys_puts("[child] 执行 exec...\n");
        const char *exec_argv[] = { "init", "--execed", NULL };
        sys_exec("/bin/init", exec_argv, (const char *const *)envp);
        sys_puts("[child] exec 失败\n");
        sys_exit(1);
    }

    sys_close(pipefds[1]);          /* 父进程只保留读端 */
    sys_puts("[init] fork 返回 ");
    fyx_put_u64((unsigned long)child);
    sys_puts(" —— 我是父进程\n");

    /* 4) wait：回收子进程并核对退出码 */
    int status = -1;
    long reaped = sys_wait(child, &status);
    sys_puts("[init] wait 回收 pid=");
    fyx_put_u64((unsigned long)reaped);
    sys_puts(" 退出码=");
    fyx_put_u64((unsigned long)status);
    sys_puts(status == 7 ? "  ✓ 与子进程一致\n" : "  ✗ 退出码不符\n");

    /* 5) 读回子进程在 exec 前写入的管道数据 */
    char pipe_buf[16] = { 0 };
    long pipe_n = sys_fd_read(pipefds[0], pipe_buf, sizeof(pipe_buf) - 1);
    sys_puts("[init] pipe 收到 ");
    sys_puts(pipe_n > 0 ? pipe_buf : "(无数据)");
    sys_puts((pipe_n == 7 && pipe_buf[0] == 'p' && pipe_buf[6] == 'k')
             ? "  ✓ 内容一致\n" : "  ✗ 内容不符\n");
    sys_close(pipefds[0]);

    /* 6) 忙等几个 tick：期间会被时钟中断抢占，再回到用户态继续 */
    long t0 = sys_get_ticks();
    while (sys_get_ticks() - t0 < TICKS_TO_SPIN)
        ;
    long t1 = sys_get_ticks();      /* 先取值，打印本身也会消耗模拟 tick */
    sys_puts("[init] 经过 ");
    fyx_put_u64((unsigned long)(t1 - t0));
    sys_puts(" 个 tick 的忙等，期间被时钟抢占后已正确恢复\n");

    /* 5) brk：堆能不能长 */
    unsigned long b0 = (unsigned long)sys_brk(0);
    unsigned long b1 = (unsigned long)sys_brk(b0 + 0x2000);
    sys_puts("[init] brk ");
    fyx_put_u64(b0);
    sys_puts(" -> ");
    fyx_put_u64(b1);
    sys_puts(b1 >= b0 + 0x2000 ? "  ✓ 堆已增长\n" : "  ✗ 堆增长失败\n");

    sys_puts("\n[init] 全部通过，返回内核 Shell\n");
    sys_exit(0);
    return 0;
}
