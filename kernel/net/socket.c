/* FYXOS — 有引用的 UDP/TCP socket、绑定端口与接收队列 */
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

#define TCP_CLOSED            0u
#define TCP_SYN_SENT          1u
#define TCP_ESTABLISHED       2u
#define TCP_CLOSE_WAIT        3u

struct socket_packet {
    u32 source_ip;
    u16 source_port;
    u16 length;
    u16 offset;                 /* TCP 流的部分读取；UDP 始终为 0 */
    u8 data[SOCKET_DATAGRAM_MAX];
};

struct fyx_socket {
    spinlock_t lock;
    u32 refs;
    u8 kind;                     /* NET_SOCK_DGRAM / NET_SOCK_STREAM */
    u8 tcp_state;
    int tcp_error;
    u16 local_port;
    bool bound;
    bool connected;
    u32 peer_ip;
    u16 peer_port;
    u32 tcp_sequence;            /* 下一个发送序号 */
    u32 tcp_ack;                 /* 下一个期望接收序号 */
    u32 read_pos;
    u32 count;
    struct socket_packet queue[SOCKET_QUEUE_DEPTH];
};

static struct fyx_socket *g_sockets[NET_MAX_SOCKETS];
static spinlock_t g_socket_lock = SPINLOCK_INIT("sockets");
static u16 g_next_ephemeral = EPHEMERAL_FIRST;

static u16 read_be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static void write_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u64 socket_ms_to_ticks(u32 ms)
{
    u64 ticks = (u64)ms * pit_frequency() / 1000;
    return ticks ? ticks : 1;
}

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
    bool reclaim = false, tcp_fin = false;
    u16 local_port = 0, peer_port = 0;
    u32 peer_ip = 0, sequence = 0, acknowledgment = 0;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    if (socket->refs && --socket->refs == 0) {
        for (u32 i = 0; i < NET_MAX_SOCKETS; i++)
            if (g_sockets[i] == socket)
                g_sockets[i] = NULL;
        reclaim = true;
        if (socket->kind == NET_SOCK_STREAM &&
            (socket->tcp_state == TCP_ESTABLISHED ||
             socket->tcp_state == TCP_CLOSE_WAIT)) {
            tcp_fin = true;
            local_port = socket->local_port;
            peer_ip = socket->peer_ip;
            peer_port = socket->peer_port;
            sequence = socket->tcp_sequence;
            acknowledgment = socket->tcp_ack;
        }
    }
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (tcp_fin)
        net_tcp_send(local_port, peer_ip, peer_port, sequence, acknowledgment,
                     NET_TCP_FIN | NET_TCP_ACK, NULL, 0);
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

static int socket_open(u8 kind, struct fyx_socket **out)
{
    if (!out)
        return -FYX_EINVAL;
    *out = NULL;
    if (!net_ready())
        return -FYX_ENETUNREACH;
    struct fyx_socket *socket = kzalloc(sizeof(*socket));
    if (!socket)
        return -FYX_ENOMEM;
    spinlock_init(&socket->lock,
                  kind == NET_SOCK_STREAM ? "tcp-socket" : "udp-socket");
    socket->refs = 1;
    socket->kind = kind;

    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = install_socket(socket);
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (rc != 0) {
        kfree(socket);
        return rc;
    }
    *out = socket;
    return FYX_OK;
}

int socket_udp_open(struct fyx_socket **out)
{
    return socket_open(NET_SOCK_DGRAM, out);
}

int socket_tcp_open(struct fyx_socket **out)
{
    return socket_open(NET_SOCK_STREAM, out);
}

long sys_socket(u32 domain, u32 type, u32 protocol)
{
    if (!net_ready())
        return -FYX_ENETUNREACH;
    if (domain != NET_AF_INET ||
        (type == NET_SOCK_DGRAM &&
         protocol != 0 && protocol != NET_IPPROTO_UDP) ||
        (type == NET_SOCK_STREAM &&
         protocol != 0 && protocol != NET_IPPROTO_TCP) ||
        (type != NET_SOCK_DGRAM && type != NET_SOCK_STREAM))
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

    struct fyx_socket *socket;
    int rc = socket_open(type, &socket);
    if (rc != 0)
        return rc;
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

int socket_udp_bind(struct fyx_socket *socket, u16 port)
{
    if (!socket)
        return -FYX_EINVAL;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = bind_locked(socket, port);
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    return rc;
}

long sys_bind(int fd, const struct fyx_sockaddr_in *address, size_t address_len)
{
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if (!address || address_len < sizeof(*address) ||
        address->family != NET_AF_INET)
        return -FYX_EINVAL;
    return socket_udp_bind(socket, read_be16((const u8 *)&address->port));
}

int socket_udp_connect(struct fyx_socket *socket, u32 ip, u16 port)
{
    if (!socket || !ip || !port)
        return -FYX_EINVAL;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    int rc = bind_locked(socket, 0);
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (rc != 0)
        return rc;
    socket->peer_ip = ip;
    socket->peer_port = port;
    socket->connected = true;
    return FYX_OK;
}

static int tcp_connect_socket(struct fyx_socket *socket, u32 ip, u16 port)
{
    if (!socket || !ip || !port)
        return -FYX_EINVAL;
    if (socket->tcp_state != TCP_CLOSED)
        return -FYX_EBUSY;
    int rc = socket_udp_bind(socket, 0);
    if (rc != 0)
        return rc;
    u32 initial = 0x46590000u + socket->local_port;
    socket->peer_ip = ip;
    socket->peer_port = port;
    socket->connected = true;
    socket->tcp_error = 0;
    socket->tcp_ack = 0;
    socket->tcp_sequence = initial + 1;
    socket->tcp_state = TCP_SYN_SENT;
    rc = net_tcp_send(socket->local_port, ip, port, initial, 0,
                      NET_TCP_SYN, NULL, 0);
    if (rc != 0)
        goto fail;

    u64 deadline = pit_ticks() + socket_ms_to_ticks(100);
    while (pit_ticks() < deadline) {
        net_poll();
        if (socket->tcp_state == TCP_ESTABLISHED)
            return FYX_OK;
        if (socket->tcp_error) {
            rc = socket->tcp_error;
            goto fail;
        }
        thread_sleep_ms(1);
    }
    rc = -FYX_ETIMEDOUT;
fail:
    socket->tcp_state = TCP_CLOSED;
    socket->connected = false;
    return rc;
}

int socket_tcp_connect(struct fyx_socket *socket, u32 ip, u16 port)
{
    if (!socket || socket->kind != NET_SOCK_STREAM)
        return -FYX_EINVAL;
    return tcp_connect_socket(socket, ip, port);
}

long sys_connect(int fd, const struct fyx_sockaddr_in *address, size_t address_len)
{
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if (!address || address_len < sizeof(*address) ||
        address->family != NET_AF_INET || !address->port)
        return -FYX_EINVAL;
    u16 port = read_be16((const u8 *)&address->port);
    if (socket->kind == NET_SOCK_STREAM)
        return tcp_connect_socket(socket, address->addr, port);
    return socket_udp_connect(socket, address->addr, port);
}

static long socket_send_to(struct fyx_socket *socket, const void *buffer,
                           size_t len, u32 ip, u16 port)
{
    if ((!buffer && len) || len > NET_MAX_UDP_PAYLOAD)
        return -FYX_ERANGE;
    if (!ip || !port)
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

long socket_udp_send(struct fyx_socket *socket, const void *buffer, size_t len)
{
    if (!socket)
        return -FYX_EBADF;
    if (!socket->connected)
        return -FYX_ENOTCONN;
    return socket_send_to(socket, buffer, len, socket->peer_ip,
                          socket->peer_port);
}

static long tcp_send_socket(struct fyx_socket *socket,
                            const void *buffer, size_t len)
{
    if (socket->tcp_state != TCP_ESTABLISHED)
        return -FYX_ENOTCONN;
    if (!len)
        return 0;
    if (!buffer || len > NET_MAX_TCP_PAYLOAD)
        return -FYX_ERANGE;
    int rc = net_tcp_send(socket->local_port, socket->peer_ip,
                          socket->peer_port, socket->tcp_sequence,
                          socket->tcp_ack, NET_TCP_PSH | NET_TCP_ACK,
                          buffer, len);
    if (rc != 0)
        return rc;
    socket->tcp_sequence += (u32)len;
    return (long)len;
}

long socket_tcp_send(struct fyx_socket *socket, const void *buffer, size_t len)
{
    if (!socket || socket->kind != NET_SOCK_STREAM)
        return -FYX_EBADF;
    return tcp_send_socket(socket, buffer, len);
}

long sys_sendto(int fd, const void *buffer, size_t len, u32 flags,
                const struct fyx_sockaddr_in *destination, size_t address_len)
{
    UNUSED(flags);
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    if (socket->kind == NET_SOCK_STREAM) {
        if (destination)
            return -FYX_EINVAL;
        return tcp_send_socket(socket, buffer, len);
    }

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
    return socket_send_to(socket, buffer, len, ip, port);
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
        packet->offset = 0;
        memcpy(packet->data, data, len);
        socket->count++;
    }
    spin_unlock(&socket->lock);
    irq_restore(irq);
    socket_release(socket);
}

void net_socket_tcp_deliver(u16 destination_port, u32 source_ip,
                            u16 source_port, u32 sequence, u32 acknowledgment,
                            u8 flags, const void *data, size_t len)
{
    if (len > NET_MAX_TCP_PAYLOAD)
        return;
    struct fyx_socket *socket = NULL;
    u64 irq = irq_save_disable();
    spin_lock(&g_socket_lock);
    for (u32 i = 0; i < NET_MAX_SOCKETS; i++) {
        struct fyx_socket *candidate = g_sockets[i];
        if (!candidate || candidate->kind != NET_SOCK_STREAM ||
            !candidate->bound || candidate->local_port != destination_port ||
            !candidate->connected || candidate->peer_ip != source_ip ||
            candidate->peer_port != source_port)
            continue;
        socket = candidate;
        socket->refs++;
        break;
    }
    spin_unlock(&g_socket_lock);
    irq_restore(irq);
    if (!socket)
        return;

    bool acknowledge = false;
    irq = irq_save_disable();
    spin_lock(&socket->lock);
    if (flags & NET_TCP_RST) {
        socket->tcp_error = -FYX_ENOTCONN;
        socket->tcp_state = TCP_CLOSED;
        socket->connected = false;
    } else if (socket->tcp_state == TCP_SYN_SENT) {
        if ((flags & (NET_TCP_SYN | NET_TCP_ACK)) ==
            (NET_TCP_SYN | NET_TCP_ACK) &&
            acknowledgment == socket->tcp_sequence) {
            socket->tcp_ack = sequence + 1;
            socket->tcp_state = TCP_ESTABLISHED;
            acknowledge = true;
        }
    } else if (socket->tcp_state == TCP_ESTABLISHED ||
               socket->tcp_state == TCP_CLOSE_WAIT) {
        if (len) {
            acknowledge = true;       /* 正确、重复或乱序都确认当前期望序号 */
            if (sequence == socket->tcp_ack &&
                socket->count < SOCKET_QUEUE_DEPTH) {
                u32 slot = (socket->read_pos + socket->count) % SOCKET_QUEUE_DEPTH;
                struct socket_packet *packet = &socket->queue[slot];
                packet->source_ip = source_ip;
                packet->source_port = source_port;
                packet->length = (u16)len;
                packet->offset = 0;
                memcpy(packet->data, data, len);
                socket->count++;
                socket->tcp_ack += (u32)len;
            }
        }
        if ((flags & NET_TCP_FIN) && socket->tcp_state == TCP_ESTABLISHED &&
            sequence + (u32)len == socket->tcp_ack) {
            socket->tcp_ack++;
            socket->tcp_state = TCP_CLOSE_WAIT;
            acknowledge = true;
        }
    }
    u32 next_sequence = socket->tcp_sequence;
    u32 acknowledgment_next = socket->tcp_ack;
    spin_unlock(&socket->lock);
    irq_restore(irq);
    if (acknowledge)
        net_tcp_send(destination_port, source_ip, source_port, next_sequence,
                     acknowledgment_next, NET_TCP_ACK, NULL, 0);
    socket_release(socket);
}

long socket_udp_recv(struct fyx_socket *socket, void *buffer, size_t len,
                     u32 flags, u32 *source_ip, u16 *source_port)
{
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
            if (source_ip)
                *source_ip = packet->source_ip;
            if (source_port)
                *source_port = packet->source_port;
            socket->read_pos = (socket->read_pos + 1) % SOCKET_QUEUE_DEPTH;
            socket->count--;
            spin_unlock(&socket->lock);
            irq_restore(irq);
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

static long tcp_recv_socket(struct fyx_socket *socket, void *buffer, size_t len,
                            u32 flags, u32 *source_ip, u16 *source_port)
{
    if (!buffer && len)
        return -FYX_EFAULT;
    if (!len)
        return 0;
    for (;;) {
        u64 irq = irq_save_disable();
        spin_lock(&socket->lock);
        if (socket->count) {
            struct socket_packet *packet = &socket->queue[socket->read_pos];
            size_t available = packet->length - packet->offset;
            size_t copied = MIN(len, available);
            memcpy(buffer, packet->data + packet->offset, copied);
            packet->offset += (u16)copied;
            if (packet->offset == packet->length) {
                socket->read_pos = (socket->read_pos + 1) % SOCKET_QUEUE_DEPTH;
                socket->count--;
            }
            if (source_ip)
                *source_ip = packet->source_ip;
            if (source_port)
                *source_port = packet->source_port;
            spin_unlock(&socket->lock);
            irq_restore(irq);
            return (long)copied;
        }
        int error = socket->tcp_error;
        bool eof = socket->tcp_state == TCP_CLOSE_WAIT;
        bool closed = socket->tcp_state == TCP_CLOSED;
        spin_unlock(&socket->lock);
        irq_restore(irq);
        if (error)
            return error;
        if (eof)
            return 0;
        if (closed)
            return -FYX_ENOTCONN;
        if (flags & NET_MSG_DONTWAIT)
            return -FYX_EAGAIN;
        net_poll();
        thread_sleep_ms(1);
    }
}

long socket_tcp_recv(struct fyx_socket *socket, void *buffer, size_t len,
                     u32 flags)
{
    if (!socket || socket->kind != NET_SOCK_STREAM)
        return -FYX_EBADF;
    return tcp_recv_socket(socket, buffer, len, flags, NULL, NULL);
}

long sys_recvfrom(int fd, void *buffer, size_t len, u32 flags,
                  struct fyx_sockaddr_in *source, size_t *address_len)
{
    struct fyx_socket *socket = fd_socket(fd);
    if (!socket)
        return -FYX_EBADF;
    u32 source_ip = 0;
    u16 source_port = 0;
    long rc = socket->kind == NET_SOCK_STREAM
        ? tcp_recv_socket(socket, buffer, len, flags, &source_ip, &source_port)
        : socket_udp_recv(socket, buffer, len, flags, &source_ip, &source_port);
    if (rc < 0)
        return rc;
    if (source) {
        memset(source, 0, sizeof(*source));
        source->family = NET_AF_INET;
        write_be16((u8 *)&source->port, source_port);
        source->addr = source_ip;
    }
    if (address_len)
        *address_len = sizeof(*source);
    return rc;
}
