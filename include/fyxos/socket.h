/* FYXOS — UDP socket 与进程 fd 的粘合层 */
#ifndef FYXOS_SOCKET_H
#define FYXOS_SOCKET_H

#include <fyxos/types.h>
#include <fyxos/net.h>

struct fyx_socket;
struct process;

void socket_ref(struct fyx_socket *socket);
void socket_release(struct fyx_socket *socket);

/* 内核网络客户端与 ring3 系统调用共用 UDP/TCP socket 和队列实现。 */
int  socket_udp_open(struct fyx_socket **out);
int  socket_udp_bind(struct fyx_socket *socket, u16 port);
int  socket_udp_connect(struct fyx_socket *socket, u32 ip, u16 port);
long socket_udp_send(struct fyx_socket *socket, const void *buffer, size_t len);
long socket_udp_recv(struct fyx_socket *socket, void *buffer, size_t len,
                     u32 flags, u32 *source_ip, u16 *source_port);
int  socket_tcp_open(struct fyx_socket **out);
int  socket_tcp_connect(struct fyx_socket *socket, u32 ip, u16 port);
long socket_tcp_send(struct fyx_socket *socket, const void *buffer, size_t len);
long socket_tcp_recv(struct fyx_socket *socket, void *buffer, size_t len,
                     u32 flags);

#endif /* FYXOS_SOCKET_H */
