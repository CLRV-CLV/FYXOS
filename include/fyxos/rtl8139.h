/* FYXOS — RTL8139 快速以太网驱动 */
#ifndef FYXOS_RTL8139_H
#define FYXOS_RTL8139_H

#include <fyxos/types.h>

typedef void (*net_rx_fn)(const void *packet, size_t len);

int  rtl8139_init(net_rx_fn handler);
bool rtl8139_present(void);
int  rtl8139_send(const void *packet, size_t len);
u32  rtl8139_poll(void);
void rtl8139_mac(u8 mac[6]);
void rtl8139_stats(u64 *tx, u64 *rx, u64 *errors);

#endif /* FYXOS_RTL8139_H */
