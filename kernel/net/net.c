/*
 * FYXOS — Ethernet / ARP / IPv4 / DHCP / ICMP / UDP
 *
 * 所有协议报文都由内核自行构造、校验与解析。虚拟对端只负责回应标准协议，
 * 不替客户机实现 DHCP 状态机、ARP 缓存或 socket。
 */
#include <fyxos/net.h>
#include <fyxos/rtl8139.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/x86_64.h>
#include <fyxos/sched.h>

#define ETH_ARP       0x0806
#define ETH_IPV4      0x0800
#define IP_ICMP       1
#define IP_UDP        17
#define NET_MAX_FRAME 1514u
#define DHCP_XID      0x46595801u
#define DHCP_MAGIC    0x63825363u
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

static u16 read_be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static u32 read_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}
static void write_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void write_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static u16 internet_checksum(const void *data, size_t len)
{
    const u8 *p = data;
    u32 sum = 0;
    while (len >= 2) {
        sum += read_be16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u16)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

static struct {
    bool present;
    bool ready;
    u8 mac[6];
    u32 ip;
    u32 gateway;
    u32 netmask;
    u32 arp_ip;
    u8 arp_mac[6];
    bool arp_valid;
    u32 dhcp_xid;
    u32 dhcp_offered_ip;
    u32 dhcp_server;
    bool dhcp_offer;
    u16 ip_ident;
    u16 ping_sequence;
    u16 ping_reply_sequence;
    u32 ping_reply_ip;
    struct net_stats stats;
} g_net;

static void mac_print(const u8 mac[6])
{
    kprintf("%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void ip_print(u32 ip)
{
    kprintf("%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF);
}

static u64 ms_to_ticks(u32 ms)
{
    u64 ticks = (u64)ms * pit_frequency() / 1000;
    return ticks ? ticks : 1;
}

bool net_present(void) { return g_net.present; }
bool net_ready(void)   { return g_net.ready; }
void net_poll(void)    { if (g_net.present) rtl8139_poll(); }

void net_get_info(struct net_info *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    memcpy(out->mac, g_net.mac, 6);
    memcpy(out->gateway_mac, g_net.arp_mac, 6);
    out->ip = g_net.ip;
    out->gateway = g_net.gateway;
    out->netmask = g_net.netmask;
    out->ready = g_net.ready;
    out->gateway_mac_valid = g_net.arp_valid;
}

void net_get_stats(struct net_stats *out)
{
    if (!out)
        return;
    *out = g_net.stats;
    rtl8139_stats(&out->tx_packets, &out->rx_packets, &out->rx_errors);
}

/* -------------------------------- ARP ----------------------------------- */
static int arp_send(u16 operation, u32 target_ip, const u8 target_mac[6],
                    const u8 ethernet_dst[6])
{
    u8 packet[14 + 28];
    memset(packet, 0, sizeof(packet));
    memcpy(packet, ethernet_dst, 6);
    memcpy(packet + 6, g_net.mac, 6);
    write_be16(packet + 12, ETH_ARP);
    u8 *arp = packet + 14;
    write_be16(arp + 0, 1);              /* Ethernet */
    write_be16(arp + 2, ETH_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, operation);
    memcpy(arp + 8, g_net.mac, 6);
    write_be32(arp + 14, g_net.ip);
    memcpy(arp + 18, target_mac, 6);
    write_be32(arp + 24, target_ip);
    return rtl8139_send(packet, sizeof(packet));
}

static int arp_resolve(u32 ip, u32 timeout_ms)
{
    static const u8 broadcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    static const u8 zero[6] = { 0 };
    u64 deadline = pit_ticks() + ms_to_ticks(timeout_ms);
    while (pit_ticks() < deadline) {
        if (g_net.arp_valid && g_net.arp_ip == ip)
            return FYX_OK;
        arp_send(1, ip, zero, broadcast);
        g_net.stats.arp_requests++;
        u64 retry = pit_ticks() + ms_to_ticks(20);
        while (pit_ticks() < retry) {
            net_poll();
            if (g_net.arp_valid && g_net.arp_ip == ip)
                return FYX_OK;
            thread_sleep_ms(1);
        }
    }
    return -FYX_ETIMEDOUT;
}

static void handle_arp(const u8 *arp, size_t len)
{
    if (len < 28 || read_be16(arp) != 1 || read_be16(arp + 2) != ETH_IPV4 ||
        arp[4] != 6 || arp[5] != 4)
        return;
    u16 operation = read_be16(arp + 6);
    u32 sender_ip = read_be32(arp + 14);
    u32 target_ip = read_be32(arp + 24);
    if (operation == 2) {
        memcpy(g_net.arp_mac, arp + 8, 6);
        g_net.arp_ip = sender_ip;
        g_net.arp_valid = true;
        g_net.stats.arp_replies++;
    } else if (operation == 1 && g_net.ready && target_ip == g_net.ip) {
        arp_send(2, sender_ip, arp + 8, arp + 8);
    }
}

/* -------------------------------- IPv4 ---------------------------------- */
static int ipv4_send(u32 destination, u8 protocol, const void *payload,
                     size_t payload_len)
{
    if (payload_len > 1480 || 14 + 20 + payload_len > NET_MAX_FRAME)
        return -FYX_ERANGE;
    u8 packet[NET_MAX_FRAME];
    memset(packet, 0, 14 + 20);
    const u8 *destination_mac = NULL;
    static const u8 broadcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

    if (destination == NET_IP_BROADCAST) {
        destination_mac = broadcast;
    } else {
        u32 next_hop = destination;
        if (g_net.ready && g_net.netmask &&
            (destination & g_net.netmask) != (g_net.ip & g_net.netmask))
            next_hop = g_net.gateway;
        int rc = arp_resolve(next_hop, 100);
        if (rc != 0)
            return rc;
        destination_mac = g_net.arp_mac;
    }
    memcpy(packet, destination_mac, 6);
    memcpy(packet + 6, g_net.mac, 6);
    write_be16(packet + 12, ETH_IPV4);

    u8 *ip = packet + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    write_be16(ip + 2, (u16)(20 + payload_len));
    write_be16(ip + 4, g_net.ip_ident++);
    write_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = protocol;
    write_be32(ip + 12, g_net.ip);
    write_be32(ip + 16, destination);
    write_be16(ip + 10, internet_checksum(ip, 20));
    memcpy(ip + 20, payload, payload_len);
    return rtl8139_send(packet, 14 + 20 + payload_len);
}

/* -------------------------------- DHCP ---------------------------------- */
static int dhcp_send(u8 message_type)
{
    u8 bootp[240 + 32];
    memset(bootp, 0, sizeof(bootp));
    bootp[0] = 1;                       /* request */
    bootp[1] = 1;                       /* Ethernet */
    bootp[2] = 6;
    write_be32(bootp + 4, g_net.dhcp_xid);
    write_be16(bootp + 10, 0x8000);     /* broadcast reply */
    memcpy(bootp + 28, g_net.mac, 6);
    write_be32(bootp + 236, DHCP_MAGIC);
    size_t option = 240;
    bootp[option++] = 53;
    bootp[option++] = 1;
    bootp[option++] = message_type;
    if (message_type == DHCP_REQUEST) {
        bootp[option++] = 50;
        bootp[option++] = 4;
        write_be32(bootp + option, g_net.dhcp_offered_ip);
        option += 4;
        bootp[option++] = 54;
        bootp[option++] = 4;
        write_be32(bootp + option, g_net.dhcp_server);
        option += 4;
    }
    bootp[option++] = 255;

    u8 udp[8 + sizeof(bootp)];
    write_be16(udp + 0, 68);
    write_be16(udp + 2, 67);
    write_be16(udp + 4, (u16)sizeof(udp));
    write_be16(udp + 6, 0);             /* IPv4 UDP checksum 可省略 */
    memcpy(udp + 8, bootp, sizeof(bootp));
    return ipv4_send(NET_IP_BROADCAST, IP_UDP, udp, sizeof(udp));
}

static u32 dhcp_option(const u8 *options, size_t len, u8 wanted)
{
    size_t i = 0;
    while (i < len && options[i] != 255) {
        u8 code = options[i++];
        if (code == 0)
            continue;
        if (i >= len)
            break;
        u8 size = options[i++];
        if (i + size > len)
            break;
        if (code == wanted && size >= 1) {
            if (size == 1)
                return options[i];
            if (size == 4)
                return read_be32(options + i);
        }
        i += size;
    }
    return 0;
}

static void handle_dhcp(const u8 *bootp, size_t len)
{
    if (len < 240 || read_be32(bootp + 4) != g_net.dhcp_xid ||
        read_be32(bootp + 236) != DHCP_MAGIC)
        return;
    u8 type = (u8)dhcp_option(bootp + 240, len - 240, 53);
    if (type == DHCP_OFFER) {
        g_net.dhcp_offered_ip = read_be32(bootp + 16);
        g_net.dhcp_server = dhcp_option(bootp + 240, len - 240, 54);
        if (!g_net.dhcp_server)
            g_net.dhcp_server = read_be32(bootp + 20);
        g_net.dhcp_offer = true;
    } else if (type == DHCP_ACK) {
        g_net.ip = read_be32(bootp + 16);
        g_net.netmask = dhcp_option(bootp + 240, len - 240, 1);
        g_net.gateway = dhcp_option(bootp + 240, len - 240, 3);
        if (!g_net.netmask)
            g_net.netmask = NET_IP(255,255,255,0);
        if (!g_net.gateway)
            g_net.gateway = g_net.dhcp_server;
        g_net.ready = true;
    }
}

/* -------------------------------- ICMP ---------------------------------- */
static int icmp_send_echo(u32 destination, u16 sequence)
{
    u8 icmp[8 + 8];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 8;
    icmp[1] = 0;
    write_be16(icmp + 4, 0x4659);
    write_be16(icmp + 6, sequence);
    memcpy(icmp + 8, "FYXOSPING", 8);
    write_be16(icmp + 2, internet_checksum(icmp, sizeof(icmp)));
    return ipv4_send(destination, IP_ICMP, icmp, sizeof(icmp));
}

int net_ping(u32 ip, u32 timeout_ms, u32 *rtt_ms)
{
    if (!g_net.ready)
        return -FYX_ENETUNREACH;
    u16 sequence = ++g_net.ping_sequence;
    u64 start = pit_ticks();
    int rc = icmp_send_echo(ip, sequence);
    if (rc != 0)
        return rc;
    u64 deadline = start + ms_to_ticks(timeout_ms);
    while (pit_ticks() < deadline) {
        net_poll();
        if (g_net.ping_reply_sequence == sequence && g_net.ping_reply_ip == ip) {
            u64 elapsed = pit_ticks() - start;
            if (rtt_ms)
                *rtt_ms = (u32)(elapsed * 1000 / pit_frequency());
            return FYX_OK;
        }
        thread_sleep_ms(1);
    }
    return -FYX_ETIMEDOUT;
}

static void handle_icmp(u32 source, const u8 *icmp, size_t len)
{
    if (len < 8 || internet_checksum(icmp, len) != 0)
        return;
    if (icmp[0] == 0 && read_be16(icmp + 4) == 0x4659) {
        g_net.ping_reply_sequence = read_be16(icmp + 6);
        g_net.ping_reply_ip = source;
        g_net.stats.icmp_echo_replies++;
    } else if (icmp[0] == 8 && g_net.ready) {
        u8 reply[1500];
        if (len > sizeof(reply))
            return;
        memcpy(reply, icmp, len);
        reply[0] = 0;
        reply[2] = reply[3] = 0;
        write_be16(reply + 2, internet_checksum(reply, len));
        ipv4_send(source, IP_ICMP, reply, len);
    }
}

/* -------------------------------- UDP ----------------------------------- */
int net_udp_send(u16 local_port, u32 remote_ip, u16 remote_port,
                 const void *data, size_t len)
{
    if (!g_net.ready || !remote_port || !local_port)
        return -FYX_ENETUNREACH;
    if (!data || len > NET_MAX_UDP_PAYLOAD)
        return -FYX_ERANGE;
    u8 udp[8 + NET_MAX_UDP_PAYLOAD];
    write_be16(udp + 0, local_port);
    write_be16(udp + 2, remote_port);
    write_be16(udp + 4, (u16)(8 + len));
    write_be16(udp + 6, 0);
    memcpy(udp + 8, data, len);
    int rc = ipv4_send(remote_ip, IP_UDP, udp, 8 + len);
    if (rc == 0)
        g_net.stats.udp_sent++;
    return rc;
}

static void handle_udp(u32 source_ip, const u8 *udp, size_t len)
{
    if (len < 8)
        return;
    u16 source_port = read_be16(udp + 0);
    u16 destination_port = read_be16(udp + 2);
    u16 udp_len = read_be16(udp + 4);
    if (udp_len < 8 || udp_len > len)
        return;
    if (destination_port == 68 && !g_net.ready) {
        handle_dhcp(udp + 8, udp_len - 8);
        return;
    }
    g_net.stats.udp_received++;
    net_socket_udp_deliver(destination_port, source_ip, source_port,
                           udp + 8, udp_len - 8);
}

static void handle_ipv4(const u8 *ip, size_t len)
{
    if (len < 20 || (ip[0] >> 4) != 4)
        return;
    u16 ihl = (u16)(ip[0] & 0x0F) * 4;
    u16 total = read_be16(ip + 2);
    if (ihl < 20 || ihl > len || total < ihl || total > len ||
        internet_checksum(ip, ihl) != 0)
        return;
    u32 destination = read_be32(ip + 16);
    u32 source = read_be32(ip + 12);
    if (g_net.ready && destination != g_net.ip &&
        destination != NET_IP_BROADCAST)
        return;
    if (ip[9] == IP_ICMP)
        handle_icmp(source, ip + ihl, total - ihl);
    else if (ip[9] == IP_UDP)
        handle_udp(source, ip + ihl, total - ihl);
}

static void handle_ethernet(const void *packet, size_t len)
{
    const u8 *eth = packet;
    if (len < 14)
        return;
    u16 type = read_be16(eth + 12);
    if (type == ETH_ARP)
        handle_arp(eth + 14, len - 14);
    else if (type == ETH_IPV4)
        handle_ipv4(eth + 14, len - 14);
}

/* -------------------------------- 初始化 --------------------------------- */
int net_init(void)
{
    memset(&g_net, 0, sizeof(g_net));
    g_net.dhcp_xid = DHCP_XID;
    g_net.ip_ident = 1;
    if (rtl8139_init(handle_ethernet) != 0) {
        kprintf("[net] 未发现 RTL8139，网络保持离线\n");
        return -FYX_ENOSYS;
    }
    g_net.present = true;
    rtl8139_mac(g_net.mac);

    dhcp_send(DHCP_DISCOVER);
    u64 deadline = pit_ticks() + ms_to_ticks(100);
    while (!g_net.dhcp_offer && pit_ticks() < deadline) {
        net_poll();
        thread_sleep_ms(1);
    }
    if (!g_net.dhcp_offer)
        return -FYX_ETIMEDOUT;

    dhcp_send(DHCP_REQUEST);
    deadline = pit_ticks() + ms_to_ticks(100);
    while (!g_net.ready && pit_ticks() < deadline) {
        net_poll();
        thread_sleep_ms(1);
    }
    if (!g_net.ready)
        return -FYX_ETIMEDOUT;

    kprintf("[net] DHCP 完成：IP=");
    ip_print(g_net.ip);
    kprintf(" 网关=");
    ip_print(g_net.gateway);
    kprintf(" 掩码=");
    ip_print(g_net.netmask);
    kprintf("\n");

    int rc = arp_resolve(g_net.gateway, 100);
    if (rc != 0)
        return rc;
    kprintf("[net] ARP 网关 ");
    ip_print(g_net.gateway);
    kprintf(" -> ");
    mac_print(g_net.arp_mac);
    kprintf("\n");

    u32 rtt = 0;
    rc = net_ping(g_net.gateway, 100, &rtt);
    if (rc != 0)
        return rc;
    kprintf("[net] ICMP 网关往返成功：%u ms\n", rtt);
    return FYX_OK;
}
