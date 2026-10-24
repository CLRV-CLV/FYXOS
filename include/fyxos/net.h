/* FYXOS — IPv4 网络栈与 UDP socket 接口 */
#ifndef FYXOS_NET_H
#define FYXOS_NET_H

#include <fyxos/types.h>

struct fyx_socket;

#define NET_IP(a,b,c,d) (((u32)(a) << 24) | ((u32)(b) << 16) | \
                         ((u32)(c) << 8) | (u32)(d))
#define NET_IP_BROADCAST 0xFFFFFFFFu

#define NET_AF_INET       2u
#define NET_SOCK_DGRAM    2u
#define NET_IPPROTO_UDP   17u
#define NET_MSG_DONTWAIT  1u
#define NET_MAX_UDP_PAYLOAD 1472u

/* 与 POSIX sockaddr_in 的 16 字节布局兼容；port/addr 为网络字节序。 */
struct fyx_sockaddr_in {
    u16 family;
    u16 port;
    u32 addr;
    u8  zero[8];
} PACKED;

struct net_info {
    u8  mac[6];
    u8  gateway_mac[6];
    u32 ip;
    u32 gateway;
    u32 dns_server;
    u32 netmask;
    bool ready;
    bool gateway_mac_valid;
};

struct net_stats {
    u64 tx_packets;
    u64 rx_packets;
    u64 rx_errors;
    u64 arp_requests;
    u64 arp_replies;
    u64 icmp_echo_replies;
    u64 udp_sent;
    u64 udp_received;
    u64 udp_dropped;
};

int  net_init(void);
void net_poll(void);
bool net_present(void);
bool net_ready(void);
void net_get_info(struct net_info *out);
void net_get_stats(struct net_stats *out);
int  net_ping(u32 ip, u32 timeout_ms, u32 *rtt_ms);
int  net_dns_resolve(const char *name, u32 *address, u32 timeout_ms);
int  net_udp_send(u16 local_port, u32 remote_ip, u16 remote_port,
                  const void *data, size_t len);

/* 网络驱动收到的 UDP 数据报，由 socket 层派发给本地端口。 */
void net_socket_udp_deliver(u16 local_port, u32 remote_ip, u16 remote_port,
                            const void *data, size_t len);

long sys_socket(u32 family, u32 type, u32 protocol);
long sys_bind(int fd, const struct fyx_sockaddr_in *addr, size_t addrlen);
long sys_connect(int fd, const struct fyx_sockaddr_in *addr, size_t addrlen);
long sys_sendto(int fd, const void *buf, size_t len, u32 flags,
                const struct fyx_sockaddr_in *dst, size_t addrlen);
long sys_recvfrom(int fd, void *buf, size_t len, u32 flags,
                  struct fyx_sockaddr_in *src, size_t *addrlen);

#endif /* FYXOS_NET_H */
