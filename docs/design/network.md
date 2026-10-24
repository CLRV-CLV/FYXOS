# FYXOS 阶段 I：RTL8139 与 IPv4/UDP/DNS 主链

本文只记录已经构建并在 Unicorn 台架实跑的实现，不把路线图中的 TCP、IPv6
或 TLS 视为已完成。

## 1. 已落地的调用链

```text
PCI 配置空间
  → RTL8139 I/O + Bus Master DMA
  → Ethernet
  → ARP / IPv4
  → DHCP（含 DNS option 6）、ICMP echo、UDP
  → 有引用的 socket fd / 内核 UDP 句柄
  → ring3 SYSCALL/SYSRET 或 DNS A 查询
```

- `kernel/drivers/rtl8139.c`：探测 `10ec:8139`，打开 I/O 与 Bus Master，执行
  复位，分配物理 RX/TX 缓冲区，设置 RX 环与 TX 描述符。接收首版采用轮询。
- RX 记录按硬件小端格式解析：低 16 位为状态，高 16 位为**包含 4 字节 CRC**
  的线长；协议栈只接收去掉 CRC 后的 Ethernet 帧。CBR 是生产者偏移，CAPR
  是消费者偏移减 16。
- `kernel/net/net.c`：自行构造和解析 Ethernet、ARP、IPv4、DHCP、ICMP、UDP
  报文，计算/校验 Internet checksum，并维护单一 ARP 下一跳缓存。DHCP ACK
  读取 option 6；缺失时以网关作为 DNS 首版回退。
- `kernel/net/socket.c`：最多 16 个 UDP socket，每 socket 8 个数据报的接收
  队列；支持显式/隐式绑定、临时端口、connect 过滤、引用计数与 fork 继承。
  ring3 fd 和内核网络客户端共用相同的分配、bind/connect、send/recv 实现。
- `kernel/net/dns.c`：通过真实内核 UDP socket 查询 A 记录，不走固定旁路端口。
- `kernel/sys/pipe.c`：文件、管道、socket 统一经过 fd 的
  `read/write/close` 分派；socket 也可使用 `sendto/recvfrom`。

## 2. 用户 ABI

网络系统调用仍为：

| 编号 | 接口 |
|---|---|
| `0x1021` | `socket(domain, type, protocol)` |
| `0x1022` | `bind(fd, sockaddr_in *, addrlen)` |
| `0x1023` | `connect(fd, sockaddr_in *, addrlen)` |
| `0x1024` | `sendto(fd, buf, len, flags, dst, addrlen)` |
| `0x1025` | `recvfrom(fd, buf, len, flags, src, addrlen *)` |

`struct fyx_sockaddr_in` 为 16 字节，`port` 按网络字节序存放，`addr` 使用
`NET_IP(A,B,C,D)` 生成的 32 位 IPv4 数值。当前只接受 `AF_INET/SOCK_DGRAM`
和 UDP；其他 domain/type 返回 `-FYX_ENOSYS`，不是静默模拟成功。DNS 当前是
内核接口与 Shell 功能，尚未增加用户态 `getaddrinfo` 系统调用。

## 3. DNS v1 的协议检查

`net_dns_resolve(name, &address, timeout_ms)` 发送单个递归 A 查询。查询名
支持字母、数字、`-`、`_`、点和最后一个根点；检查空标签、63 字节标签上限
与 255 字节编码上限。

响应必须来自 connected socket 的 DNS/53 对端，并满足：

- 事务 ID、单问题、QNAME（ASCII 大小写不敏感）、QTYPE 与 QCLASS 匹配；
- QR/opcode、RCODE、TC 和所有 question/answer 边界有效；
- 压缩名支持 pointer，但限制 32 次跳转、255 字节解码结果并拒绝环路；
- 只接受 `IN/A` 且 `rdlength=4` 的地址；其他记录会安全跳过。

错误 ID/问题返回内部 `EAGAIN` 并继续等待；NXDOMAIN 和 NOERROR 无 A 记录为
`-FYX_ENOENT`（-8），超时为 -18，畸形/截断/环路报文为 `-FYX_EPROTO`（-21）。
无效本地主机名为 -1。当前不缓存、不重传、不递归追 CNAME，也不在 UDP 截断
后用 TCP 重试。

## 4. Shell 与可见状态

- `ifconfig`：显示 MAC、IPv4、掩码、网关、DNS、ARP 结果与协议统计。
- `resolve <主机名> [毫秒]`：通过 DHCP 下发的服务器查询并打印 IPv4。
- `ping [主机名/IPv4] [毫秒]`：默认 ping 网关；主机名先经 DNS 解析，再执行
  原来的 ICMP echo 往返。

`ifconfig` 会先执行一次 `net_poll()`，因此同一命令也覆盖空 RX 环路径。

## 5. 台架不是“直接返回成功”

`tools/rtl8139.py` 实现两部分：

1. PCI 配置端口、RTL8139 I/O 寄存器、TX DMA 读取与 RX DMA 写入；
2. 独立的标准协议对端：DHCP offer/ack、DNS、网关 ARP reply、ICMP echo reply、
   UDP/9 echo。

对端只解析客户机实际写入 TX DMA 的报文并生成回应。DHCP 状态、ARP 缓存、
IP/UDP 构造、socket 队列、DNS 编码/解码和用户 ABI 都在 FYXOS 内实现。台架的
网络钩子并入既有 `port_in/port_out` 入口，OUT 值从 RAX 按宽度取得，规避
Unicorn 的钩子值滞后问题。

默认测试配置：

- 客户机：`52:54:00:12:34:56`，DHCP 获得 `10.0.2.15/24`；
- 网关/DNS/对端：`52:54:00:65:02:02`，`10.0.2.2`；
- ring3 init 绑定 UDP/40000，向 UDP/9 发送 `fyxos-ring3-udp` 并校验回应；
- `example.test` 先返回错误 ID/不同地址，再返回正确 ID、压缩名的
  `93.184.216.34`，只有忽略第一帧才会通过；
- `missing.test`、`empty.test`、`broken.test`、`unknown.test` 分别覆盖
  NXDOMAIN、无 A、压缩指针自环与无响应超时；
- `gateway.test` 解析为 `10.0.2.2`，随后完成真实 ICMP echo。

## 6. 验收

```bash
make image
make test-network   # DNS 3 项：正向/异常报文、ping 主机名组合链路
make test           # full/quick + DNS + 工具链/Git/CLI/图形/存储全部回归
```

DNS 专项日志写入 `build/network-test/dns.log`。full-boot 与 quick 的原有检查
仍包含 DHCP、ARP、ICMP、ring3 UDP、Shell `ifconfig/ping` 与正常关机。

## 7. 尚未完成的边界

- 仅 RTL8139、IPv4、UDP、ICMP echo、DHCP 与 DNS A v1；无 E1000/VirtIO-net。
- 无 IPv6、TCP、TLS、IP 分片重组和 UDP checksum 生成/校验。
- DNS 无缓存、重传、AAAA、CNAME 递归、TCP 回退、搜索域和用户 getaddrinfo。
- ARP 目前只缓存一个下一跳；DHCP 首版不做租约续期与多接口配置。
- RTL8139 首版轮询接收，无 NIC IRQ；大 RX 环绕回与完整错误恢复仍需加强。
- socket 队列有固定容量，用户指针完整边界检查仍属于后续安全加固。
