/*
 * FYXOS — 阶段 G 的最小管道 fd 接口
 *
 * 这是进程间通信的第一块，不是控制台 write 的替代品：
 *   SYS_LOG_WRITE 仍然负责把字节写到内核控制台；
 *   SYS_READ/SYS_WRITE 则操作当前进程的管道 fd。
 */
#ifndef FYXOS_PIPE_H
#define FYXOS_PIPE_H

#include <fyxos/types.h>
#include <fyxos/process.h>

#define FYX_FD_READ   0x01u
#define FYX_FD_WRITE  0x02u

long sys_pipe(int *fds);                         /* fds[0]=读，fds[1]=写 */
long sys_fd_read(int fd, void *buf, size_t len);
long sys_fd_write(int fd, const void *buf, size_t len);
long sys_close(int fd);

/* fork 时复制 fd 表并增加管道端点引用 */
void pipe_clone_fds(struct process *child, const struct process *parent);
/* exit/process_free 时关闭所有端点，唤醒读者看到 EOF */
void pipe_close_all(struct process *p);

#endif /* FYXOS_PIPE_H */
