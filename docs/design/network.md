# FYXOS 阶段 I：RTL8139 与 IPv4/UDP 主链

本文只记录已经构建并在 Unicorn 台架实跑的实现，不把路线图中的 TCP、IPv6、
DNS 或 TLS 视为已完成。

## 1. 已落地的调用链

```text
PCI 配置空间
  → RTL8139 I/O + Bus Master DMA
  → Ethernet
  → ARP / IPv4
  → DHCP 客户端、ICMP echo、UDP
  → 有引用的 socket fd
  → ring3 SYSCALL/SYSRET
```

- `kernel/drivers/rtl8139.c`：探测 `10ec:8139`，打开 I/O 与 Bus Master，执行
  复位，分配物理 RX/TX 缓冲区，设置 RX 环与 TX 描述符。接收首版采用轮询。
- RX 记录按硬件小端格式解析：低 16 位为状态，高 16 位为**包含 4 字节 CRC**
  的线长；协议栈只接收去掉 CRC 后的 Ethernet 帧。CBR 是生产者偏移，CAPR
  是消费者偏移减 16。
- `kernel/net/net.c`：自行构造和解析 Ethernet、ARP、IPv4、DHCP、ICMP、UDP
  报文，计算/校验 Internet checksum，并维护单一 ARP 下一跳缓存。
- `kernel/net/socket.c`：最多 16 个 UDP socket，每 socket 8 个数据报的接收
  队列；支持显式/隐式绑定、临时端口、connect 过滤、引用计数与 fork 继承。
- `kernel/sys/pipe.c`：文件、管道、socket 统一经过 fd 的
  `read/write/close` 分派；socket 也可使用 `sendto/recvfrom`。

## 2. 用户 ABI

新增系统调用：

| 编号 | 接口 |
|---|---|
| `0x1021` | `socket(domain, type, protocol)` |
| `0x1022` | `bind(fd, sockaddr_in *, addrlen)` |
| `0x1023` | `connect(fd, sockaddr_in *, addrlen)` |
| `0x1024` | `sendto(fd, buf, len, flags, dst, addrlen)` |
| `0x1025` | `recvfrom(fd, buf, len, flags, src, addrlen *)` |

`struct fyx_sockaddr_in` 为 16 字节，`port` 按网络字节序存放，`addr` 使用
`NET_IP(A,B,C,D)` 生成的 32 位 IPv4 数值。当前只接受 `AF_INET/SOCK_DGRAM`
和 UDP；其他 domain/type 返回 `-FYX_ENOSYS`，不是静默模拟成功。

## 3. Shell 与可见状态

- `ifconfig`：显示 MAC、IPv4、掩码、网关、ARP 结果以及 RX/TX/UDP/ICMP 统计。
- `ping [IPv4] [毫秒]`：默认 ping DHCP 网关，也可指定点分十进制地址和超时。

`ifconfig` 会先执行一次 `net_poll()`，因此同一命令也覆盖空 RX 环路径。

## 4. 台架不是“直接返回成功”

`tools/rtl8139.py` 实现两部分：

1. PCI 配置端口、RTL8139 I/O 寄存器、TX DMA 读取与 RX DMA 写入；
2. 独立的标准协议对端：DHCP offer/ack、网关 ARP reply、ICMP echo reply、
   UDP/9 echo。

对端只解析客户机实际写入 TX DMA 的报文并生成回应。DHCP 状态、ARP 缓存、
IP/UDP 构造、socket 队列和用户 ABI 都在 FYXOS 内实现。台架的网络钩子并入
既有 `port_in/port_out` 入口，OUT 值从 RAX 按宽度取得，规避 Unicorn 的钩子
值滞后问题。

默认测试配置：

- 客户机：`52:54:00:12:34:56`，DHCP 获得 `10.0.2.15/24`；
- 网关/对端：`52:54:00:65:02:02`，`10.0.2.2`；
- ring3 init 绑定 UDP/40000，向 `10.0.2.2:9` 发送 `fyxos-ring3-udp`，
  校验回应的源地址、源端口和完整载荷。

## 5. 验收

```bash
make image
.venv/bin/python tools/fyxemu.py \
  --image build/fyxos.img --kernel build/fyxos.elf --mode full-boot \
  --keys 'ifconfig;ping;shutdown' --keys-after '[init] 全部通过' \
  --test --max-seconds 60

make test          # full-boot + quick + 存储/回滚/断电套件
make test-storage  # 独立重跑 H/M 回归
```

full-boot 与 quick 的端到端检查均包含 DHCP、ARP、ICMP、ring3 UDP、Shell
`ifconfig/ping`、原有用户态/文件/进程链路和正常关机。存储套件继续验证写点
1/9/16/17 的断电结果及 `/home` 保留回滚。

## 6. 尚未完成的边界

- 仅 RTL8139、IPv4、UDP、ICMP echo 与 DHCP；无 E1000/VirtIO-net。
- 无 IPv6、TCP、DNS、TLS、IP 分片重组和 UDP checksum 生成/校验。
- ARP 目前只缓存一个下一跳；DHCP 首版不做租约续期与多接口配置。
- RTL8139 首版轮询接收，无 NIC IRQ；大 RX 环绕回与完整错误恢复仍需加强。
- socket 队列有固定容量，用户指针完整边界检查仍属于后续安全加固。
