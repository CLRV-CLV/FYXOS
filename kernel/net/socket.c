/* FYXOS — 有引用的 UDP socket、绑定端口与数据报接收队列 */
#include <fyxos/socket.h>
#include <fyxos/process.h>
#include <fyxos/pipe.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/sched.h>
#include <fyxos/x86_64.h>

#define NET_MAX_SOCKETS       16u
#define SOCKET_QUEUE_DEPTH    8u
#define SOCKET_DATAGRAM_MAX   NET_MAX_UDP_PAYLOAD
#define EPHEMERAL_FIRST       49152u
#define EPHEMERAL_LAST        65535u

struct socket_packet {
    u32 source_ip;
    u16 source_port;
    u16 length;
    u8 data[SOCKET_DATAGRAM_MAX];
};

struct fyx_socket {
    spinlock_t lock;
    u32 refs;
    u16 local_port;
    bool bound;
    bool connected;
    u32 peer_ip;
    u16 peer_port;
    u32 read_pos;
    u32 count;
    struct socket_packet queue[SOCKET_QUEUE_DEPTH];
};

static struct fyx_socket *g_sockets[NET_MAX_SOCKETS];
static spinlock_t g_socket_lock = SPINLOCK_INIT("sockets");
static u16 g_next_ephemeral = EPHEMERAL_FIRST;

static u16 read_be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static void write_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static struct fyx_socket *fd_socket(int fd)
{
    struct process *p = current_process();
    if (!p || fd < 0 || fd >= PROC_MAX_FDS ||
        p->fds[fd].type != FYX_FD_SOCKET)
        return NULL;
    return p->fds[fd].socket;
}

void socket_ref(struct fyx_socket *socket)
{
    if (!socket)
        return;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    if (socket->refs)
        socket->refs++;
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
}

void socket_release(struct fyx_socket *socket)
{
    if (!socket)
        return;
    bool reclaim = false;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    if (socket->refs && --socket->refs == 0) {
        for (u32 i = 0; i < NET_MAX_SOCKETS; i++)
            if (g_sockets[i] == socket)
                g_sockets[i] = NULL;
        reclaim = true;
    }
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (reclaim)
        kfree(socket);
}

static int install_socket(struct fyx_socket *socket)
{
    for (u32 i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!g_sockets[i]) {
            g_sockets[i] = socket;
            return FYX_OK;
        }
    }
    return -FYX_ENOSPC;
}

long sys_socket(u32 domain, u32 type, u32 protocol)
{
    if (!net_ready())
        return -FYX_ENETUNREACH;
    if (domain != NET_AF_INET || type != NET_SOCK_DGRAM ||
        (protocol != 0 && protocol != NET_IPPROTO_UDP))
        return -FYX_ENOSYS;
    struct process *p = current_process();
    if (!p)
        return -FYX_EINVAL;
    int fd;
    for (fd = 0; fd < PROC_MAX_FDS; fd++)
        if (p->fds[fd].type == FYX_FD_NONE)
            break;
    if (fd == PROC_MAX_FDS)
        return -FYX_ENOSPC;

    struct fyx_socket *socket = kzalloc(sizeof(*socket));
    if (!socket)
        return -FYX_ENOMEM;
    spinlock_init(&socket->lock, "udp-socket");
    socket->refs = 1;

    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = install_socket(socket);
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (rc != 0) {
        kfree(socket);
        return rc;
    }
    p->fds[fd].socket = socket;
    p->fds[fd].type = FYX_FD_SOCKET;
    p->fds[fd].flags = FYX_FD_READ | FYX_FD_WRITE;
    return fd;
}

static bool port_in_use(u16 port, const struct fyx_socket *except)
{
    for (u32 i = 0; i < NET_MAX_SOCKETS; i++) {
        struct fyx_socket *other = g_sockets[i];
        if (other && other != except && other->bound &&
            other->local_port == port)
            return true;
    }
    return false;
}

/* g_socket_lock 必须已持有。 */
static int bind_locked(struct fyx_socket *socket, u16 port)
{
    if (socket->bound)
        return (!port || socket->local_port == port)
            ? FYX_OK : -FYX_EADDRINUSE;
    if (!port) {
        u32 attempts = EPHEMERAL_LAST - EPHEMERAL_FIRST + 1;
        while (attempts--) {
            port = g_next_ephemeral++;
            if (g_next_ephemeral < EPHEMERAL_FIRST)
                g_next_ephemeral = EPHEMERAL_FIRST;
            if (!port_in_use(port, socket))
                break;
            port = 0;
        }
        if (!port)
            return -FYX_ENOSPC;
    } else if (port_in_use(port, socket)) {
        return -FYX_EADDRINUSE;
    }
    socket->local_port = port;
    socket->bound = true;
    return FYX_OK;
}

long sys_bind(int fd, const struct fyx_sockaddr_in *address, size_t address_len)
{
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if (!address || address_len < sizeof(*address) ||
        address->family != NET_AF_INET)
        return -FYX_EINVAL;
    u16 port = read_be16((const u8 *)&address->port);
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = bind_locked(socket, port);
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    return rc;
}

long sys_connect(int fd, const struct fyx_sockaddr_in *address, size_t address_len)
{
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if (!address || address_len < sizeof(*address) ||
        address->family != NET_AF_INET || !address->port)
        return -FYX_EINVAL;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = bind_locked(socket, 0);
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (rc != 0)
        return rc;
    socket->peer_ip = address->addr;
    socket->peer_port = read_be16((const u8 *)&address->port);
    socket->connected = true;
    return FYX_OK;
}

long sys_sendto(int fd, const void *buffer, size_t len, u32 flags,
                const struct fyx_sockaddr_in *destination, size_t address_len)
{
    UNUSED(flags);
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if ((!buffer && len) || len > NET_MAX_UDP_PAYLOAD)
        return -FYX_ERANGE;

    u32 ip;
    u16 port;
    if (destination) {
        if (address_len < sizeof(*destination) ||
            destination->family != NET_AF_INET)
            return -FYX_EINVAL;
        ip = destination->addr;
        port = read_be16((const u8 *)&destination->port);
    } else {
        if (!socket->connected)
            return -FYX_ENOTCONN;
        ip = socket->peer_ip;
        port = socket->peer_port;
    }
    if (!port)
        return -FYX_EINVAL;

    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = bind_locked(socket, 0);
    u16 local_port = socket->local_port;
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (rc != 0)
        return rc;
    rc = net_udp_send(local_port, ip, port, buffer, len);
    return rc == 0 ? (long)len : rc;
}

void net_socket_udp_deliver(u16 destination_port, u32 source_ip,
                            u16 source_port, const void *data, size_t len)
{
    if (len > SOCKET_DATAGRAM_MAX)
        return;
    struct fyx_socket *socket = NULL;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    for (u32 i = 0; i < NET_MAX_SOCKETS; i++) {
        struct fyx_socket *candidate = g_sockets[i];
        if (!candidate || !candidate->bound ||
            candidate->local_port != destination_port)
            continue;
        if (candidate->connected &&
            (candidate->peer_ip != source_ip ||
             candidate->peer_port != source_port))
            continue;
        socket = candidate;
        socket->refs++;              /* 在全局锁内固定生命周期 */
        break;
    }
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (!socket)
        return;

    irq = irq_save_disable();
    spin_lock(&socket->lock);
    if (socket->count < SOCKET_QUEUE_DEPTH) {
        u32 slot = (socket->read_pos + socket->count) % SOCKET_QUEUE_DEPTH;
        struct socket_packet *packet = &socket->queue[slot];
        packet->source_ip = source_ip;
        packet->source_port = source_port;
        packet->length = (u16)len;
        memcpy(packet->data, data, len);
        socket->count++;
    }
    spin_unlock(&socket->lock);
    irq_restore(irq);
    socket_release(socket);
}

long sys_recvfrom(int fd, void *buffer, size_t len, u32 flags,
                  struct fyx_sockaddr_in *source, size_t *address_len)
{
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if (!buffer && len)
        return -FYX_EFAULT;

    for (;;) {
        u64 irq = irq_save_disable();
        spin_lock(&socket->lock);
        if (socket->count) {
            struct socket_packet *packet = &socket->queue[socket->read_pos];
            size_t copied = MIN(len, (size_t)packet->length);
            memcpy(buffer, packet->data, copied);
            u32 source_ip = packet->source_ip;
            u16 source_port = packet->source_port;
            socket->read_pos = (socket->read_pos + 1) % SOCKET_QUEUE_DEPTH;
            socket->count--;
            spin_unlock(&socket->lock);
            irq_restore(irq);
            if (source) {
                memset(source, 0, sizeof(*source));
                source->family = NET_AF_INET;
                write_be16((u8 *)&source->port, source_port);
                source->addr = source_ip;
            }
            if (address_len)
                *address_len = sizeof(*source);
            return (long)copied;
        }
        spin_unlock(&socket->lock);
        irq_restore(irq);
        if (flags & NET_MSG_DONTWAIT)
            return -FYX_EAGAIN;
        net_poll();
        thread_sleep_ms(1);
    }
}
