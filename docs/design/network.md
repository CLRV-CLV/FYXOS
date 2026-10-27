# FYXOS 阶段 I：RTL8139 与 IPv4/UDP/TCP/DNS 主链

本文只记录已经构建并在 Unicorn 台架实跑的实现，不把完整 TCP、IPv6 或 TLS
视为已完成。

## 1. 已落地的调用链

```text
PCI 配置空间
  → RTL8139 I/O + Bus Master DMA
  → Ethernet
  → ARP / IPv4
  → DHCP（含 DNS option 6）、ICMP echo、UDP、TCP
  → 有引用的 socket fd / 内核 UDP 句柄
  → ring3 SYSCALL/SYSRET、流式 read/write 或 DNS A 查询
```

- `kernel/drivers/rtl8139.c`：探测 `10ec:8139`，打开 I/O 与 Bus Master，执行
  复位，分配物理 RX/TX 缓冲区，设置 RX 环与 TX 描述符。接收首版采用轮询。
- RX 记录按硬件小端格式解析：低 16 位为状态，高 16 位为**包含 4 字节 CRC**
  的线长；协议栈只接收去掉 CRC 后的 Ethernet 帧。CBR 是生产者偏移，CAPR
  是消费者偏移减 16。
- `kernel/net/net.c`：自行构造和解析 Ethernet、ARP、IPv4、DHCP、ICMP、UDP、
  TCP 报文，计算/校验 Internet 与 UDP/TCP 伪首部 checksum，维护单一 ARP
  下一跳缓存。DHCP ACK 读取 option 6；缺失时以网关作为 DNS 首版回退。
- `kernel/net/socket.c`：最多 16 个 UDP/TCP socket，每 socket 8 个接收队列
  项；支持显式/隐式绑定、临时端口、connect 过滤、引用计数与 fork 继承。
  UDP 使用数据报队列；TCP 支持同一接收项的部分读取和流式 EOF。
- `kernel/net/dns.c`：通过真实内核 UDP socket 查询 A 记录；遇 TC 后复用内核
  TCP socket 回退，不走固定旁路端口或另一套握手实现。
- `kernel/sys/pipe.c`：文件、管道、socket 统一经过 fd 的
  `read/write/close` 分派；socket 也可使用 `sendto/recvfrom`。

## 2. 用户 ABI

网络系统调用仍为 5 个，没有为 TCP 增加旁路 ABI：

| 编号 | 接口 |
|---|---|
| `0x1021` | `socket(domain, type, protocol)` |
| `0x1022` | `bind(fd, sockaddr_in *, addrlen)` |
| `0x1023` | `connect(fd, sockaddr_in *, addrlen)` |
| `0x1024` | `sendto(fd, buf, len, flags, dst, addrlen)` |
| `0x1025` | `recvfrom(fd, buf, len, flags, src, addrlen *)` |

`struct fyx_sockaddr_in` 为 16 字节，`port` 按网络字节序存放，`addr` 使用
`NET_IP(A,B,C,D)` 生成的 32 位 IPv4 数值。UDP 接受 `SOCK_DGRAM/IPPROTO_UDP`；
TCP 主动客户端接受 `SOCK_STREAM/IPPROTO_TCP`，protocol 为 0 时按 type 选择。
TCP 也走现有 `read/write/close`。其他 domain/type 返回 `-FYX_ENOSYS`，不是
静默模拟成功。DNS 当前是内核接口与 Shell 功能，尚未提供 POSIX getaddrinfo。

## 3. TCP 主动客户端 v1

已实现并实跑：

- 主动 open：绑定临时端口，发送 SYN，校验匹配四元组的 SYN-ACK 与 ACK 序号，
  然后发送 ACK 并进入 ESTABLISHED；连接超时或 RST 会返回错误；
- 发送固定 20 字节头的 PSH/ACK 数据段，维护下一发送序号与期望接收序号；
  SYN 与单个未确认数据段按固定 20ms 重试、最多发送三次，数据收到匹配 ACK
  后才向 `write` 返回成功；已有未确认段时返回 `EBUSY`；
- 接收只按期望序号入队，发送 ACK；队列满、重复或乱序不会伪报已接收；
- 单段最多 1460 字节；应用用较小 buffer 读取时保留未消费部分，而不是像
  UDP 一样丢弃数据报剩余字节；
- 接收 FIN 时推进确认号，排队数据 drain 后 `read` 返回 0；最后一个 fd
  引用关闭时发送 FIN/ACK；
- `ifconfig` 显示 TCP 收、发和 checksum/格式丢弃统计。

这是受限客户端，不是完整 TCP：没有 listen/accept/server、同时打开、多段
发送未确认队列、自适应 RTO、拥塞/流量控制、乱序重组、选择确认、窗口缩放、
TCP options、MSS 协商或完整 FIN/TIME_WAIT 状态机。

## 4. UDP checksum

所有 guest 发出的 UDP 报文（包括 DHCP 与 DNS）都计算 IPv4 伪首部和 UDP
checksum；收到非零 checksum 的 UDP 报文也必须通过同一伪首部校验。为兼容
IPv4 的可选校验行为，checksum 字段为零的报文仍可接收，但不因此跳过非零坏
checksum 的拒绝。坏报文计入 `ifconfig` 的 UDP 丢包统计，不交付 socket。

## 5. DNS v1 的协议检查

`net_dns_resolve(name, &address, timeout_ms)` 发送单个递归 A 查询。查询名
支持字母、数字、`-`、`_`、点和最后一个根点；检查空标签、63 字节标签上限
与 255 字节编码上限。

响应必须来自 connected socket 的 DNS/53 对端，并满足：

- 事务 ID、单问题、QNAME（ASCII 大小写不敏感）、QTYPE 与 QCLASS 匹配；
- QR/opcode、RCODE、TC 和所有 question/answer 边界有效；
- 压缩名支持 pointer，但限制 32 次跳转、255 字节解码结果并拒绝环路；
- 只接受 `IN/A` 且 `rdlength=4` 的地址；其他记录会安全跳过。

错误 ID/问题返回内部 `EAGAIN` 并继续等待；NXDOMAIN 和 NOERROR 无 A 记录为
`-FYX_ENOENT`（-8），超时为 -18，畸形/环路报文为 `-FYX_EPROTO`（-21）。
无效本地主机名为 -1。只有合法 UDP 响应中的 TC 会触发 TCP 回退：连接同一
DNS/53，发送两字节网络序长度前缀和原查询，再按长度读取完整响应；读取允许
多次部分 TCP recv，提前 EOF、超时和长度超过 1472 字节都会报错。其他畸形
UDP 报文不会被误判为截断并重试。UDP 无响应时在总超时的一半以相同事务 ID
重传一次，不扩大调用者的总等待预算。成功 A 记录（包括 TCP 回退结果）进入
8 项缓存，键为 DNS 服务器与规范化小写 QNAME，根点和大小写不影响命中；
TTL=0 不缓存，到期后重新查询并更新。当前不做负缓存，也不递归追 CNAME。

## 6. Shell 与可见状态

- `ifconfig`：显示 MAC、IPv4、掩码、网关、DNS、ARP 结果与 UDP/TCP 统计。
- `resolve <主机名> [毫秒]`：通过 DHCP 下发的服务器查询并打印 IPv4。
- `ping [主机名/IPv4] [毫秒]`：默认 ping 网关；主机名先经 DNS 解析，再执行
  原来的 ICMP echo 往返。

`ifconfig` 会先执行一次 `net_poll()`，因此同一命令也覆盖空 RX 环路径。

## 7. 台架不是“直接返回成功”

`tools/rtl8139.py` 实现两部分：

1. PCI 配置端口、RTL8139 I/O 寄存器、TX DMA 读取与 RX DMA 写入；
2. 独立的标准协议对端：DHCP、DNS、ARP、ICMP、UDP/9 echo 和有状态 TCP/7。

对端只解析客户机实际写入 TX DMA 的报文并生成回应。DHCP 状态、ARP 缓存、
IP/UDP/TCP 构造、socket 队列、DNS 编码/解码和用户 ABI 都在 FYXOS 内实现。
台架网络钩子并入既有 `port_in/port_out` 入口，OUT 值从 RAX 按宽度取得。

默认测试配置：

- 客户机：`52:54:00:12:34:56`，DHCP 获得 `10.0.2.15/24`；
- 网关/DNS/对端：`52:54:00:65:02:02`，`10.0.2.2`；
- ring3 init 绑定 UDP/40000，向 UDP/9 发送 `fyxos-ring3-udp` 并校验回应；
  对端先注入同内容但 UDP checksum 错误的 echo，再发送正确帧，坏帧误收会使
  内容比较失败；
- TCP/8 对 SYN 返回标准 RST/ACK，ring3 必须得到连接错误；TCP/7 先发出
  checksum 错误且序号不同的 SYN-ACK，再发正确帧，坏帧若未被丢弃会使后续
  echo 失败；对端丢弃首个 TCP/53 SYN 和首个 TCP/7 数据段，强制实测重传，
  同时校验 guest 伪首部 checksum、SEQ/ACK，echo 携带 FIN；
- ring3 TCP 每次只读 3 字节，验证部分读取，读完全部 `fyxos-ring3-tcp` 后
  还必须得到 FIN-EOF，最后 close 发出 FIN；
- `example.test` 先返回错误 ID/不同地址，再返回正确 ID、压缩名的
  `93.184.216.34`，只有忽略第一帧才会通过；
- `tcpfallback.test` 的 UDP 应答设置 TC，只有继续完成 TCP/53 握手并发送正确
  两字节长度前缀，才会经 TCP 得到 `93.184.216.35`；
- `retry.test` 只在第二次查询时应答，首命令必须重传才成功；随后
  `RETRY.TEST.` 必须命中缓存，因为对端不会再应答。`expire.test` 的 TTL=1，
  首次返回 `.37`，等待 1100ms 后必须重新查询并得到 `.38`；
- `missing.test`、`empty.test`、`broken.test`、`unknown.test` 分别覆盖
  NXDOMAIN、无 A、压缩指针自环与无响应超时；
- `gateway.test` 解析为 `10.0.2.2`，随后完成真实 ICMP echo。

## 8. 验收

```bash
make image
make test-network   # 网络 6 项；基础自检同时要求 ring3 UDP/TCP 全部通过
make test           # full/quick + 网络 + 工具链/Git/CLI/图形/存储全部回归
```

DNS 专项日志写入 `build/network-test/dns.log`。full-boot 与 quick 均直接检查
`[init] ring3 TCP socket 通过`，并保留 DHCP、ARP、ICMP、ring3 UDP、Shell
`ifconfig/ping` 与正常关机检查。

## 9. 尚未完成的边界

- 仅 RTL8139、IPv4、UDP、ICMP echo、DHCP、DNS A v1 与 TCP 主动客户端 v1；
  无 E1000/VirtIO-net。
- TCP 无 listen/server、自适应 RTO、拥塞控制、乱序重组、options 和完整关闭
  状态机；重传仅限固定间隔的 SYN 与单个未确认数据段。
- 无 IPv6、TLS 和 IP 分片重组。
- DNS 无负缓存、AAAA、CNAME 递归、搜索域和 POSIX getaddrinfo；TCP 回退
  当前每次查询只接收一个、最大 1472 字节的 DNS 报文。
- ARP 目前只缓存一个下一跳；DHCP 首版不做租约续期与多接口配置。
- RTL8139 首版轮询接收，无 NIC IRQ；大 RX 环绕回与完整错误恢复仍需加强。
- socket 队列有固定容量，用户指针完整边界检查仍属于后续安全加固。
