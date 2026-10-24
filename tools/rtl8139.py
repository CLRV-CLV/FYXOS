"""RTL8139 + 小型标准 IPv4 对端（只用于 FYXOS Unicorn 台架）。

设备侧模拟 PCI 配置空间、I/O 寄存器与 DMA；协议对端只接收客户机报文并
生成标准回应，不替客户机维护 ARP、DHCP 或 socket 状态。
"""
from __future__ import annotations

from collections import deque
import struct

from unicorn.x86_const import UC_X86_REG_RAX


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ipv4_packet(source: bytes, destination: bytes, protocol: int,
                payload: bytes, ident: int = 1) -> bytes:
    header = bytearray(20)
    header[0] = 0x45
    struct.pack_into("!H", header, 2, 20 + len(payload))
    struct.pack_into("!H", header, 4, ident & 0xFFFF)
    header[8] = 64
    header[9] = protocol
    header[12:16] = source
    header[16:20] = destination
    struct.pack_into("!H", header, 10, checksum(bytes(header)))
    return bytes(header) + payload


def ethernet(destination: bytes, source: bytes, ethertype: int,
             payload: bytes) -> bytes:
    return destination + source + struct.pack("!H", ethertype) + payload


class IPv4Peer:
    """10.0.2.0/24 上的 DHCP、网关 ARP、ICMP echo 与 UDP/9 echo。"""

    def __init__(self, guest_mac: bytes):
        self.guest_mac = guest_mac
        self.peer_mac = bytes.fromhex("525400650202")
        self.guest_ip = bytes((10, 0, 2, 15))
        self.server_ip = bytes((10, 0, 2, 2))
        self.netmask = bytes((255, 255, 255, 0))
        self.ident = 1
        self.dhcp_offers = 0
        self.dhcp_acks = 0
        self.arp_replies = 0
        self.icmp_replies = 0
        self.udp_echoes = 0

    def _ip(self, source: bytes, destination: bytes, protocol: int,
            payload: bytes) -> bytes:
        self.ident += 1
        return ipv4_packet(source, destination, protocol, payload, self.ident)

    def _udp(self, source_ip: bytes, destination_ip: bytes,
             source_port: int, destination_port: int, payload: bytes) -> bytes:
        udp = struct.pack("!HHHH", source_port, destination_port,
                          8 + len(payload), 0) + payload
        return self._ip(source_ip, destination_ip, 17, udp)

    def _dhcp(self, request: bytes, message_type: int) -> bytes:
        bootp = bytearray(300)
        bootp[0] = 2                         # reply
        bootp[1] = 1                         # Ethernet
        bootp[2] = 6
        bootp[4:8] = request[4:8]            # xid
        bootp[10:12] = request[10:12]        # broadcast flag
        bootp[16:20] = self.guest_ip         # yiaddr
        bootp[20:24] = self.server_ip        # siaddr
        bootp[28:34] = request[28:34]        # chaddr
        bootp[236:240] = b"\x63\x82\x53\x63"
        options = bytearray((53, 1, message_type, 54, 4)) + self.server_ip
        options += bytes((1, 4)) + self.netmask
        options += bytes((3, 4)) + self.server_ip
        options += bytes((51, 4)) + struct.pack("!I", 86400)
        options += b"\xff"
        bootp[240:240 + len(options)] = options
        return self._udp(self.server_ip, b"\xff\xff\xff\xff", 67, 68,
                         bytes(bootp))

    def handle(self, frame: bytes) -> list[bytes]:
        if len(frame) < 14:
            return []
        source_mac = frame[6:12]
        ethertype = struct.unpack("!H", frame[12:14])[0]
        payload = frame[14:]

        if ethertype == 0x0806 and len(payload) >= 28:
            operation = struct.unpack("!H", payload[6:8])[0]
            sender_ip = payload[14:18]
            target_ip = payload[24:28]
            if operation == 1 and target_ip == self.server_ip:
                self.arp_replies += 1
                arp = bytearray(28)
                struct.pack_into("!HH", arp, 0, 1, 0x0800)
                arp[4:6] = b"\x06\x04"
                struct.pack_into("!H", arp, 6, 2)
                arp[8:14] = self.peer_mac
                arp[14:18] = self.server_ip
                arp[18:24] = source_mac
                arp[24:28] = sender_ip
                return [ethernet(source_mac, self.peer_mac, 0x0806,
                                 bytes(arp))]
            return []

        if ethertype != 0x0800 or len(payload) < 20:
            return []
        ihl = (payload[0] & 0x0F) * 4
        if ihl < 20 or len(payload) < ihl:
            return []
        total = struct.unpack("!H", payload[2:4])[0]
        if total < ihl or total > len(payload):
            return []
        source_ip = payload[12:16]
        destination_ip = payload[16:20]
        protocol = payload[9]
        body = payload[ihl:total]

        if protocol == 1 and len(body) >= 8 and body[0] == 8:
            if destination_ip != self.server_ip:
                return []
            reply = bytearray(body)
            reply[0] = 0
            reply[2:4] = b"\0\0"
            struct.pack_into("!H", reply, 2, checksum(bytes(reply)))
            self.icmp_replies += 1
            return [ethernet(source_mac, self.peer_mac, 0x0800,
                             self._ip(self.server_ip, source_ip, 1,
                                      bytes(reply)))]

        if protocol != 17 or len(body) < 8:
            return []
        source_port, destination_port, udp_len = struct.unpack("!HHH",
                                                                  body[:6])
        if udp_len < 8 or udp_len > len(body):
            return []
        udp_payload = body[8:udp_len]

        if destination_port == 67 and source_port == 68 and len(udp_payload) >= 240:
            if udp_payload[236:240] != b"\x63\x82\x53\x63":
                return []
            options = udp_payload[240:]
            message_type = 0
            i = 0
            while i < len(options) and options[i] != 255:
                code = options[i]
                i += 1
                if code == 0:
                    continue
                if i >= len(options):
                    break
                size = options[i]
                i += 1
                if i + size > len(options):
                    break
                if code == 53 and size == 1:
                    message_type = options[i]
                i += size
            if message_type == 1:
                self.dhcp_offers += 1
                return [ethernet(b"\xff" * 6, self.peer_mac, 0x0800,
                                 self._dhcp(udp_payload, 2))]
            if message_type == 3:
                self.dhcp_acks += 1
                return [ethernet(b"\xff" * 6, self.peer_mac, 0x0800,
                                 self._dhcp(udp_payload, 5))]
            return []

        if destination_port == 9 and destination_ip == self.server_ip:
            self.udp_echoes += 1
            return [ethernet(source_mac, self.peer_mac, 0x0800,
                             self._udp(self.server_ip, source_ip, 9,
                                       source_port, udp_payload))]
        return []


class RTL8139Device:
    """RTL8139 的最小真实寄存器/DMA 模型。"""

    IO_BASE = 0xC000
    RX_RING_SIZE = 32768
    RX_OK = 0x0001
    TX_OWN = 1 << 13
    TX_TOK = 1 << 2                   # TSD.TOK

    def __init__(self, emu, quiet: bool = False):
        self.emu = emu
        self.uc = None
        self.quiet = quiet
        self.mac = bytes.fromhex("525400123456")
        self.peer = IPv4Peer(self.mac)
        self.pci_addr = 0
        self.command = 0
        self.rx_base = 0
        self.rx_capr = 0xFFF0
        self.rx_prod = 0
        self.rx_available = 0
        self.rx_pending = deque()
        self.rx_crc = 0
        self.imr = 0
        self.isr = 0
        self.cmd = 0
        self.tx_addr = [0] * 4
        self.tx_status = [0] * 4
        self.regs = {}
        self.tx_frames = 0
        self.rx_frames = 0
        self.dropped = 0

    # ------------------------------------------------------------ PCI 配置
    def _config_dword(self, offset: int) -> int:
        offset &= 0xFC
        if offset == 0x00:
            return 0x813910EC             # vendor 10EC, device 8139
        if offset == 0x04:
            return self.command & 0xFFFF
        if offset == 0x08:
            return 0x02000001             # Ethernet controller, revision 1
        if offset == 0x10:
            return self.IO_BASE | 1        # I/O BAR
        if offset == 0x3C:
            return 10                      # IRQ line 10
        return 0

    def _pci_read(self, port: int, size: int) -> int:
        offset = (self.pci_addr & 0xFC) + (port - 0xCFC)
        value = self._config_dword(offset)
        shift = (offset & 3) * 8
        return (value >> shift) & ((1 << (size * 8)) - 1)

    def _pci_write(self, port: int, size: int, value: int):
        offset = (self.pci_addr & 0xFC) + (port - 0xCFC)
        if offset == 4:
            self.command = value & 0xFFFF
        elif offset == 0x10:
            self.IO_BASE = value & ~3

    # ------------------------------------------------------------ RX DMA
    def _pump_rx(self):
        if not self.rx_base:
            return
        while self.rx_pending and self.rx_available < 32:
            frame = self.rx_pending[0]
            wire_length = len(frame) + 4       # 硬件长度含 CRC
            total = (4 + wire_length + 3) & ~3
            if total > self.RX_RING_SIZE:
                self.rx_pending.popleft()
                self.dropped += 1
                continue
            if self.rx_prod + total > self.RX_RING_SIZE:
                # 测试流量很小；绕回时从新环起点写，避免跨记录边界。
                self.rx_prod = 0
            header = self.RX_OK | (wire_length << 16)
            record = struct.pack("<I", header) + frame + b"\0" * 4
            record += b"\0" * (total - len(record))
            self.emu.phys_write(self.rx_base + self.rx_prod, record)
            self.rx_prod = (self.rx_prod + total) % self.RX_RING_SIZE
            self.rx_pending.popleft()
            self.rx_available += 1
            self.rx_frames += 1
            self.isr |= 1                    # ROK

    def _receive(self, frame: bytes):
        if len(frame) > 1518:
            self.dropped += 1
            return
        self.rx_pending.append(frame)
        self._pump_rx()

    # ------------------------------------------------------------ TX DMA
    def _transmit(self, slot: int, value: int):
        length = value & 0x1FFF
        address = self.tx_addr[slot]
        if not address or length < 14 or length > 1792:
            self.tx_status[slot] = 0
            self.dropped += 1
            return
        frame = self.emu.phys_read(address, length)
        self.tx_frames += 1
        self.tx_status[slot] = self.TX_OWN | self.TX_TOK
        for response in self.peer.handle(frame):
            self._receive(response)

    # ------------------------------------------------------------ 寄存器
    def _read_register(self, offset: int, size: int) -> int:
        if 0 <= offset < 6:
            return self.mac[offset] if size == 1 else 0
        if 0x10 <= offset <= 0x1C and (offset & 3) == 0:
            return self.tx_status[(offset - 0x10) // 4]
        if 0x20 <= offset <= 0x2C and (offset & 3) == 0:
            return self.tx_addr[(offset - 0x20) // 4]
        if offset == 0x30:
            return self.rx_base
        if offset == 0x37:
            return self.cmd
        if offset == 0x38:
            return self.rx_capr
        if offset == 0x3A:
            self._pump_rx()
            # CBR 是 RX 生产者偏移；空环时它与消费者偏移相等，
            # 不能返回特殊哨兵，否则驱动会把空环当成坏记录。
            return self.rx_prod
        if offset == 0x3C:
            return self.imr
        if offset == 0x3E:
            self._pump_rx()
            return self.isr
        if offset == 0x44:
            return self.regs.get(offset, 0)
        return self.regs.get(offset, 0)

    def _write_register(self, offset: int, size: int, value: int):
        if offset == 0x30:
            self.rx_base = value & 0xFFFFFFFC
            self.rx_prod = 0
            self.rx_available = 0
            self.rx_capr = 0xFFF0
            return
        if offset == 0x37:
            if value & 0x10:                 # RST
                self.cmd = 0
                self.isr = 0
            else:
                self.cmd = value & 0xFF
            return
        if offset == 0x38:
            new_capr = value & 0xFFFF
            if new_capr != self.rx_capr and self.rx_available:
                self.rx_available -= 1
            self.rx_capr = new_capr
            return
        if offset == 0x3C:
            self.imr = value & 0xFFFF
            return
        if offset == 0x3E:
            self.isr &= ~(value & 0xFFFF)
            return
        if 0x20 <= offset <= 0x2C and (offset & 3) == 0:
            self.tx_addr[(offset - 0x20) // 4] = value
            return
        if 0x10 <= offset <= 0x1C and (offset & 3) == 0:
            self._transmit((offset - 0x10) // 4, value)
            return
        self.regs[offset] = value

    # ------------------------------------------------------------ Unicorn hooks
    def handles(self, port: int) -> bool:
        return (port == 0xCF8 or 0xCFC <= port < 0xD00 or
                self.IO_BASE <= port < self.IO_BASE + 0x100)

    def _io_in(self, uc, port: int, size: int, user_data=None):
        if 0xCFC <= port < 0xD00:
            if (self.pci_addr & 0x80000000) and (self.pci_addr & 0x00FFFF00) == 0:
                uc.reg_write(UC_X86_REG_RAX, self._pci_read(port, size))
            else:
                uc.reg_write(UC_X86_REG_RAX, (1 << (size * 8)) - 1)
            return
        if self.IO_BASE <= port < self.IO_BASE + 0x100:
            value = self._read_register(port - self.IO_BASE, size)
            uc.reg_write(UC_X86_REG_RAX, value)
            if getattr(self.emu, "trace_io", False):
                print("NET  in%d  port=0x%04x -> 0x%x" %
                      (size * 8, port, value))

    def _io_out(self, uc, port: int, size: int, value: int, user_data=None):
        # 与台架既有 ATA/串口路径一致：OUT 钩子的 value 在部分 Unicorn
        # 构建中可能滞后，实际值从 RAX 按指令宽度取回。
        value = uc.reg_read(UC_X86_REG_RAX) & ((1 << (size * 8)) - 1)
        if port == 0xCF8:
            self.pci_addr = value
            return
        if 0xCFC <= port < 0xD00:
            if (self.pci_addr & 0x80000000) and (self.pci_addr & 0x00FFFF00) == 0:
                self._pci_write(port, size, value)
            return
        if self.IO_BASE <= port < self.IO_BASE + 0x100:
            if getattr(self.emu, "trace_io", False):
                print("NET  out%d port=0x%04x <- 0x%x" %
                      (size * 8, port, value))
            self._write_register(port - self.IO_BASE, size, value)
