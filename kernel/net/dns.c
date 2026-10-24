/* FYXOS — 通过真实 UDP socket 的 DNS/IPv4 A 记录客户端。 */
#include <fyxos/net.h>
#include <fyxos/socket.h>
#include <fyxos/lib.h>
#include <fyxos/sched.h>
#include <fyxos/x86_64.h>

#define DNS_PORT             53u
#define DNS_HEADER_SIZE      12u
#define DNS_MAX_NAME         255u
#define DNS_MAX_POINTERS     32u
#define DNS_TYPE_A           1u
#define DNS_CLASS_IN         1u

static u16 g_dns_next_id = 0x4658;

static u16 read_be16(const u8 *p) { return ((u16)p[0] << 8) | p[1]; }
static u32 read_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}
static void write_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static u64 dns_ms_to_ticks(u32 ms)
{
    u64 ticks = (u64)ms * pit_frequency() / 1000;
    return ticks ? ticks : 1;
}

static int dns_build_query(const char *name, u16 id, u8 *query,
                           size_t *query_len)
{
    if (!name || !*name || !query || !query_len)
        return -FYX_EINVAL;
    size_t length = strlen(name);
    if (length > 253)
        return -FYX_ERANGE;
    memset(query, 0, DNS_HEADER_SIZE);
    write_be16(query + 0, id);
    write_be16(query + 2, 0x0100);       /* RD：请求递归解析 */
    write_be16(query + 4, 1);            /* 一个问题 */
    size_t output = 0;                   /* 相对 QNAME 起点的编码长度 */
    size_t i = 0;
    while (i < length) {
        size_t start = i;
        while (i < length && name[i] != '.') {
            char c = name[i];
            if (!isalpha(c) && !isdigit(c) && c != '-' && c != '_')
                return -FYX_EINVAL;
            i++;
        }
        size_t label = i - start;
        if (!label || label > 63)
            return -FYX_EINVAL;
        if (output + 1 + label + 1 > DNS_MAX_NAME)
            return -FYX_ERANGE;
        query[DNS_HEADER_SIZE + output++] = (u8)label;
        memcpy(query + DNS_HEADER_SIZE + output, name + start, label);
        output += label;
        if (i < length) {
            i++;                         /* 点；允许最后一个根点 */
            if (i == length)
                break;
        }
    }
    if (output + 1 > DNS_MAX_NAME)
        return -FYX_ERANGE;
    query[DNS_HEADER_SIZE + output++] = 0;
    write_be16(query + DNS_HEADER_SIZE + output, DNS_TYPE_A);
    write_be16(query + DNS_HEADER_SIZE + output + 2, DNS_CLASS_IN);
    *query_len = DNS_HEADER_SIZE + output + 4;
    return FYX_OK;
}

/* 解码一个可能压缩的域名；offset 始终推进原报文中的名字字段。 */
static int dns_read_name(const u8 *packet, size_t length, size_t *offset,
                         u8 *decoded, size_t *decoded_len)
{
    size_t pos = *offset;
    size_t produced = 0;
    u32 pointers = 0;
    bool jumped = false;
    for (;;) {
        if (pos >= length)
            return -FYX_EPROTO;
        u8 label = packet[pos];
        if ((label & 0xC0u) == 0xC0u) {
            if (pos + 1 >= length)
                return -FYX_EPROTO;
            u16 pointer = (u16)((label & 0x3Fu) << 8) | packet[pos + 1];
            if (pointer >= length || ++pointers > DNS_MAX_POINTERS)
                return -FYX_EPROTO;
            if (!jumped)
                *offset = pos + 2;
            jumped = true;
            pos = pointer;
            continue;
        }
        if (label & 0xC0u)
            return -FYX_EPROTO;
        pos++;
        if (!label) {
            if (produced + 1 > DNS_MAX_NAME)
                return -FYX_EPROTO;
            if (decoded)
                decoded[produced] = 0;
            produced++;
            if (!jumped)
                *offset = pos;
            if (decoded_len)
                *decoded_len = produced;
            return FYX_OK;
        }
        if (label > 63 || pos + label > length ||
            produced + 1 + label + 1 > DNS_MAX_NAME)
            return -FYX_EPROTO;
        if (decoded) {
            decoded[produced] = label;
            memcpy(decoded + produced + 1, packet + pos, label);
        }
        produced += 1 + label;
        pos += label;
    }
}

static u8 ascii_lower(u8 c)
{
    return c >= 'A' && c <= 'Z' ? (u8)(c + ('a' - 'A')) : c;
}

static int dns_parse_reply(const u8 *packet, size_t length, u16 expected_id,
                           const u8 *query, size_t query_len, u32 *address)
{
    if (length < 2)
        return -FYX_EPROTO;
    if (read_be16(packet) != expected_id)
        return -FYX_EAGAIN;              /* 迟到或其他事务的响应 */
    if (length < DNS_HEADER_SIZE)
        return -FYX_EPROTO;
    u16 flags = read_be16(packet + 2);
    if (!(flags & 0x8000u) || (flags & 0x7800u))
        return -FYX_EPROTO;
    if (read_be16(packet + 4) != 1)
        return -FYX_EPROTO;

    size_t offset = DNS_HEADER_SIZE;
    u8 question[DNS_MAX_NAME];
    size_t question_len = 0;
    int rc = dns_read_name(packet, length, &offset,
                           question, &question_len);
    if (rc != 0)
        return rc;
    size_t query_name_len = query_len - DNS_HEADER_SIZE - 4;
    if (question_len != query_name_len)
        return -FYX_EAGAIN;
    for (size_t i = 0; i < question_len; i++)
        if (ascii_lower(question[i]) !=
            ascii_lower(query[DNS_HEADER_SIZE + i]))
            return -FYX_EAGAIN;
    if (offset + 4 > length)
        return -FYX_EPROTO;
    if (read_be16(packet + offset) != DNS_TYPE_A ||
        read_be16(packet + offset + 2) != DNS_CLASS_IN)
        return -FYX_EAGAIN;
    offset += 4;

    u16 rcode = flags & 0x000Fu;
    if (rcode == 3)
        return -FYX_ENOENT;
    if (rcode)
        return -FYX_EIO;
    if (flags & 0x0200u)                 /* UDP 截断：v1 不伪装完整结果 */
        return -FYX_EPROTO;

    u16 answers = read_be16(packet + 6);
    for (u16 i = 0; i < answers; i++) {
        rc = dns_read_name(packet, length, &offset, NULL, NULL);
        if (rc != 0)
            return rc;
        if (offset + 10 > length)
            return -FYX_EPROTO;
        u16 type = read_be16(packet + offset);
        u16 class = read_be16(packet + offset + 2);
        u16 rdlength = read_be16(packet + offset + 8);
        offset += 10;
        if (offset + rdlength > length)
            return -FYX_EPROTO;
        if (type == DNS_TYPE_A && class == DNS_CLASS_IN && rdlength == 4) {
            *address = read_be32(packet + offset);
            return FYX_OK;
        }
        offset += rdlength;
    }
    return -FYX_ENOENT;                  /* NOERROR 但没有可用 A 记录 */
}

int net_dns_resolve(const char *name, u32 *address, u32 timeout_ms)
{
    if (!address)
        return -FYX_EINVAL;
    *address = 0;
    u16 id = ++g_dns_next_id;
    u8 query[DNS_HEADER_SIZE + DNS_MAX_NAME + 4];
    size_t query_len = 0;
    int rc = dns_build_query(name, id, query, &query_len);
    if (rc != 0)
        return rc;
    if (!net_ready())
        return -FYX_ENETUNREACH;
    struct net_info info;
    net_get_info(&info);
    if (!info.dns_server)
        return -FYX_ENETUNREACH;

    struct fyx_socket *socket = NULL;
    rc = socket_udp_open(&socket);
    if (rc != 0)
        return rc;
    rc = socket_udp_connect(socket, info.dns_server, DNS_PORT);
    if (rc == 0) {
        long sent = socket_udp_send(socket, query, query_len);
        rc = sent == (long)query_len ? FYX_OK
            : (sent < 0 ? (int)sent : -FYX_EIO);
    }
    if (rc != 0) {
        socket_release(socket);
        return rc;
    }

    if (!timeout_ms)
        timeout_ms = 100;
    u64 deadline = pit_ticks() + dns_ms_to_ticks(timeout_ms);
    u8 reply[NET_MAX_UDP_PAYLOAD];
    rc = -FYX_ETIMEDOUT;
    while (pit_ticks() < deadline) {
        net_poll();
        long received = socket_udp_recv(socket, reply, sizeof(reply),
                                        NET_MSG_DONTWAIT, NULL, NULL);
        if (received >= 0) {
            rc = dns_parse_reply(reply, (size_t)received, id,
                                 query, query_len, address);
            if (rc != -FYX_EAGAIN)
                break;
        } else if (received != -FYX_EAGAIN) {
            rc = (int)received;
            break;
        } else {
            thread_sleep_ms(1);
        }
    }
    if (rc == -FYX_EAGAIN)
        rc = -FYX_ETIMEDOUT;
    socket_release(socket);
    return rc;
}
