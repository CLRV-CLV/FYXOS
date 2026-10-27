/* FYXOS — Win32 导入桩背后的最小服务分表（不是完整 Windows API） */
#include <fyxos/win64.h>
#include <fyxos/process.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>

static u64 g_win32_unknown;

u64 win32_syscall_unknown_count(void)
{
    return g_win32_unknown;
}

long win32_syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3,
                            u64 a4, u64 a5, u64 a6)
{
    (void)a6;
    switch (nr) {
    case SYS_WIN32_GET_STD_HANDLE:
        /* FYXOS 的控制台伪句柄；绝不把它当作 Linux fd 或内核指针。 */
        if ((i32)a1 == -11 || (i32)a1 == -12)
            return (long)(i64)(i32)a1;
        return -1;                      /* INVALID_HANDLE_VALUE */
    case SYS_WIN32_WRITE_FILE: {
        /* hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten,
         * lpOverlapped。异步 OVERLAPPED 与文件句柄表尚未纳入本纵切片。 */
        if (a5 || ((i64)a1 != -11 && (i64)a1 != -12) ||
            (a3 && !a2) || a3 > 1024 * 1024 || (a3 && !a4))
            return 0;
        if (a4)
            *(u32 *)(uintptr_t)a4 = 0;
        console_write((const char *)(uintptr_t)a2, (size_t)a3);
        if (a4)
            *(u32 *)(uintptr_t)a4 = (u32)a3;
        return 1;
    }
    case SYS_WIN32_EXIT_PROCESS:
        sys_exit((int)(u32)a1);
        return 0;                       /* 不会到达 */
    default:
        g_win32_unknown++;
        kprintf("[win32] 未实现服务 nr=0x%llx，返回失败\n",
                (unsigned long long)nr);
        return 0;
    }
}
