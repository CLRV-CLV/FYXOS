/* FYXOS — UDP socket 与进程 fd 的粘合层 */
#ifndef FYXOS_SOCKET_H
#define FYXOS_SOCKET_H

#include <fyxos/types.h>
#include <fyxos/net.h>

struct fyx_socket;
struct process;

void socket_ref(struct fyx_socket *socket);
void socket_release(struct fyx_socket *socket);

#endif /* FYXOS_SOCKET_H */
