/*
 * FYXOS — RTL8139 PCI 网卡驱动（轮询接收、DMA 收发）
 *
 * 台架与真机看到同一组标准 PCI/RTL8139 寄存器。RX/TX 缓冲区来自 PMM，
 * 给设备的是物理地址，CPU 通过 HHDM 访问，不把虚拟地址误当 DMA 地址。
 */
#include <fyxos/rtl8139.h>
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/x86_64.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define RTL_VENDOR      0x10EC
#define RTL_DEVICE      0x8139

#define RTL_IDR0        0x00
#define RTL_TXSTATUS0   0x10
#define RTL_TXADDR0     0x20
#define RTL_RXBUF       0x30
#define RTL_COMMAND     0x37
#define RTL_CAPR        0x38
#define RTL_CBR         0x3A
#define RTL_INTRMASK    0x3C
#define RTL_INTRSTATUS  0x3E
#define RTL_TXCONFIG    0x40
#define RTL_RXCONFIG    0x44
#define RTL_CONFIG1     0x52

#define RTL_CMD_RESET   0x10
#define RTL_CMD_RX      0x08
#define RTL_CMD_TX      0x04
#define RTL_RX_OK       0x0001
#define RTL_TX_OK       0x0004
#define RTL_RCR_CONFIG  0x8F      /* AAP/AAM/AB/APM + WRAP */

#define RTL_RX_SIZE     32768u
#define RTL_RX_ALLOC    (RTL_RX_SIZE + 16u)
#define RTL_RX_ALLOC_FRAMES 10u
#define RTL_MAX_PACKET  1518u

static struct {
    bool present;
    u16 io_base;
    u8 mac[6];
    phys_addr_t rx_allocation;
    phys_addr_t rx_phys;
    u8 *rx_virt;
    phys_addr_t tx_phys;
    u8 *tx_virt;
    u32 rx_offset;
    net_rx_fn rx_handler;
    u64 tx_packets;
    u64 rx_packets;
    u64 errors;
} g_rtl;

static u32 pci_read32(u8 bus, u8 device, u8 function, u8 offset)
{
    u32 address = 0x80000000u | ((u32)bus << 16) |
                  ((u32)device << 11) | ((u32)function << 8) |
                  (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(u8 bus, u8 device, u8 function, u8 offset, u32 value)
{
    u32 address = 0x80000000u | ((u32)bus << 16) |
                  ((u32)device << 11) | ((u32)function << 8) |
                  (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

static int rtl_pci_probe(u8 *device_out)
{
    for (u8 device = 0; device < 32; device++) {
        u32 id = pci_read32(0, device, 0, 0);
        if ((id & 0xFFFF) == RTL_VENDOR && (id >> 16) == RTL_DEVICE) {
            *device_out = device;
            return FYX_OK;
        }
        if (id == 0xFFFFFFFFu || id == 0)
            continue;
    }
    return -FYX_ENOSYS;
}

static inline u16 rtl_read16(u8 reg) { return inw(g_rtl.io_base + reg); }
static inline u32 rtl_read32(u8 reg) { return inl(g_rtl.io_base + reg); }

int rtl8139_init(net_rx_fn handler)
{
    memset(&g_rtl, 0, sizeof(g_rtl));
    g_rtl.rx_handler = handler;
    u8 device;
    if (rtl_pci_probe(&device) != 0)
        return -FYX_ENOSYS;

    u32 bar0 = pci_read32(0, device, 0, 0x10);
    if (!(bar0 & 1))
        return -FYX_EIO;
    g_rtl.io_base = (u16)(bar0 & 0xFFFC);

    /* PCI command：打开 I/O 空间与 Bus Master DMA。 */
    u32 command = pci_read32(0, device, 0, 0x04);
    pci_write32(0, device, 0, 0x04, (command & 0xFFFF0000u) | 0x0005u);

    /* 预留一页用于 8 KiB 对齐；剩余 36 KiB 足以容纳 32 KiB+16 字节。 */
    g_rtl.rx_allocation = pmm_alloc_frames(RTL_RX_ALLOC_FRAMES);
    g_rtl.tx_phys = pmm_alloc_frame();
    if (g_rtl.rx_allocation == PMM_INVALID || g_rtl.tx_phys == PMM_INVALID)
        return -FYX_ENOMEM;
    g_rtl.rx_phys = ALIGN_UP(g_rtl.rx_allocation, 8192u);
    if (g_rtl.rx_phys + RTL_RX_ALLOC >
        g_rtl.rx_allocation + RTL_RX_ALLOC_FRAMES * PAGE_SIZE)
        return -FYX_ENOMEM;
    g_rtl.rx_virt = phys_to_virt(g_rtl.rx_phys);
    g_rtl.tx_virt = phys_to_virt(g_rtl.tx_phys);
    memset(g_rtl.rx_virt, 0, RTL_RX_ALLOC);

    outb(g_rtl.io_base + RTL_COMMAND, RTL_CMD_RESET);
    bool reset_done = false;
    for (u32 i = 0; i < 1000000; i++) {
        if (!(inb(g_rtl.io_base + RTL_COMMAND) & RTL_CMD_RESET)) {
            reset_done = true;
            break;
        }
        io_wait();
    }
    if (!reset_done)
        return -FYX_EIO;

    for (u32 i = 0; i < 6; i++)
        g_rtl.mac[i] = inb(g_rtl.io_base + RTL_IDR0 + i);

    outb(g_rtl.io_base + RTL_CONFIG1, 0x00);
    outl(g_rtl.io_base + RTL_RXBUF, (u32)g_rtl.rx_phys);
    outw(g_rtl.io_base + RTL_CAPR, 0xFFF0u);
    outw(g_rtl.io_base + RTL_INTRMASK, 0);       /* 首版采用轮询，不靠 IRQ */
    outw(g_rtl.io_base + RTL_INTRSTATUS, 0xFFFFu);
    outl(g_rtl.io_base + RTL_TXCONFIG, 0x00000600u);
    outl(g_rtl.io_base + RTL_RXCONFIG, RTL_RCR_CONFIG);
    outb(g_rtl.io_base + RTL_COMMAND, RTL_CMD_RX | RTL_CMD_TX);
    g_rtl.rx_offset = 0;
    g_rtl.present = true;

    kprintf("[net] RTL8139 PCI 设备就绪：I/O=0x%x MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            g_rtl.io_base, g_rtl.mac[0], g_rtl.mac[1], g_rtl.mac[2],
            g_rtl.mac[3], g_rtl.mac[4], g_rtl.mac[5]);
    return FYX_OK;
}

bool rtl8139_present(void)
{
    return g_rtl.present;
}

void rtl8139_mac(u8 mac[6])
{
    if (mac)
        memcpy(mac, g_rtl.mac, 6);
}

int rtl8139_send(const void *packet, size_t len)
{
    if (!g_rtl.present || !packet || len < 14 || len > RTL_MAX_PACKET)
        return -FYX_EINVAL;
    memcpy(g_rtl.tx_virt, packet, len);
    barrier();
    outl(g_rtl.io_base + RTL_TXADDR0, (u32)g_rtl.tx_phys);
    outl(g_rtl.io_base + RTL_TXSTATUS0, (u32)len);

    for (u32 i = 0; i < 1000000; i++) {
        u32 status = rtl_read32(RTL_TXSTATUS0);
        if (status & RTL_TX_OK) {
            g_rtl.tx_packets++;
            return FYX_OK;
        }
        if (status & 0x40000000u) {              /* TX 错误 */
            g_rtl.errors++;
            return -FYX_EIO;
        }
        io_wait();
    }
    g_rtl.errors++;
    return -FYX_EIO;
}

u32 rtl8139_poll(void)
{
    if (!g_rtl.present)
        return 0;
    u32 handled = 0;
    u16 cbr = rtl_read16(RTL_CBR);
    while (g_rtl.rx_offset != cbr) {
        if (g_rtl.rx_offset >= RTL_RX_SIZE) {
            g_rtl.rx_offset = 0;
            continue;
        }
        u8 *record = g_rtl.rx_virt + g_rtl.rx_offset;
        u16 status = (u16)record[0] | ((u16)record[1] << 8);
        u16 length = (u16)record[2] | ((u16)record[3] << 8);
        if (!(status & RTL_RX_OK) || length < 4 || length > RTL_MAX_PACKET + 4) {
            g_rtl.errors++;
            g_rtl.rx_offset = cbr;               /* 丢弃损坏记录，避免死循环 */
            break;
        }
        size_t payload_len = length - 4;         /* 尾部 CRC 不交给协议栈 */
        if (g_rtl.rx_handler)
            g_rtl.rx_handler(record + 4, payload_len);
        g_rtl.rx_packets++;
        handled++;

        u32 next = (g_rtl.rx_offset + 4u + length + 3u) & ~3u;
        g_rtl.rx_offset = next % RTL_RX_SIZE;
        outw(g_rtl.io_base + RTL_CAPR, (u16)((g_rtl.rx_offset - 16u) & 0xFFFFu));
    }
    /* ROK=1、TOK=4；轮询路径仍清掉状态，防止以后开 IRQ 时积累旧事件。 */
    u16 interrupt = rtl_read16(RTL_INTRSTATUS);
    if (interrupt)
        outw(g_rtl.io_base + RTL_INTRSTATUS, interrupt);
    return handled;
}

void rtl8139_stats(u64 *tx, u64 *rx, u64 *errors)
{
    if (tx) *tx = g_rtl.tx_packets;
    if (rx) *rx = g_rtl.rx_packets;
    if (errors) *errors = g_rtl.errors;
}
