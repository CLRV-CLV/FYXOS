/* FYXOS — Win64 PE32+ 与最小 Win32 服务 ABI */
#ifndef FYXOS_WIN64_H
#define FYXOS_WIN64_H

#include <fyxos/types.h>

#define WIN32_STUB_BASE       0x0000000060000000ull
#define WIN32_STUB_STRIDE     64u
#define SYS_WIN32_GET_STD_HANDLE 0x2001
#define SYS_WIN32_WRITE_FILE     0x2002
#define SYS_WIN32_EXIT_PROCESS   0x2003

struct process;
int process_load_pe(struct process *p, const void *image, size_t size);
long win32_syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3,
                            u64 a4, u64 a5, u64 a6);
u64 win32_syscall_unknown_count(void);

#endif /* FYXOS_WIN64_H */
