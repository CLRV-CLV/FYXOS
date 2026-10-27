/*
 * FYXOS — 管道与统一 fd 接口
 *
 * SYS_LOG_WRITE 负责内核控制台；SYS_READ/SYS_WRITE 则按当前进程 fd 的
 * 类型分派到管道或 VFS 文件。此头文件保留 pipe 名称以兼容既有进程代码。
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
