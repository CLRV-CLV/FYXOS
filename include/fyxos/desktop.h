/* FYXOS — 首版软件合成桌面 */
#ifndef FYXOS_DESKTOP_H
#define FYXOS_DESKTOP_H

#include <fyxos/types.h>

int  desktop_start(void);
int  desktop_next_window(void);
int  desktop_toggle_theme(void);
int  desktop_run(void);
void desktop_print_info(void);

#endif /* FYXOS_DESKTOP_H */
