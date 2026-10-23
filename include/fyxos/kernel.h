/*
 * FYXOS — 内核核心设施：日志 / 控制台 / 恐慌 / 断言
 */
#ifndef FYXOS_KERNEL_H
#define FYXOS_KERNEL_H

#include <fyxos/types.h>

/* ============================ 内核日志环形缓冲 ============================
 * 这是一块**始终位于物理内存**中的环形缓冲区，是所有输出的第一落点。
 * 作用：
 *   1. 控制台驱动尚未初始化时也能保留早期输出（等价于 Linux 的 early printk）
 *   2. 崩溃/重启后仍可被引导器或调试器读出（事后分析）
 *   3. fyxemu 台架直接读它，不依赖任何端口/寄存器同步（见 docs/05-testing.md）
 * ------------------------------------------------------------------------ */
#define FYX_LOG_MAGIC 0x314C5958u   /* "XYL1" */
#define FYX_LOG_SIZE  16384u

struct fyx_log {
    u32     magic;
    u32     size;                   /* 缓冲区容量 */
    u32     head;                   /* 已写入总字节数（单调递增，可回绕定位） */
    u32     dropped;                /* 因无人消费而丢弃的字节数（此处恒为 0） */
    u32     write_count;            /* 写入调用次数，用于台架判断是否推进 */
    u32     panic_flag;             /* 发生 panic 时置 1 */
    u32     reserved[2];
    char    buf[FYX_LOG_SIZE];
} PACKED;

extern struct fyx_log g_klog;

void log_init(void);
void log_putc(char c);
void log_write(const char *s, size_t n);

/* ================================ 控制台 ================================= */
/* 输出后端位掩码 */
#define CONSOLE_LOG     0x01u        /* 内核日志环形缓冲（总是开启） */
#define CONSOLE_SERIAL  0x02u        /* 16550 UART */
#define CONSOLE_VGA     0x04u        /* VGA 文本模式帧缓冲 */

void console_init(void);
void console_register_backends(u32 mask);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_set_color(u8 fg, u8 bg);

/* ============================== panic / 断言 ============================= */
#define PANIC_MSG_MAX 256

struct panic_record {
    u32  happened;
    char message[PANIC_MSG_MAX];
    u64  rip;
    u64  rsp;
    u64  cr2;
    u64  rflags;
} PACKED;

extern struct panic_record g_panic_record;

void NORETURN panic(const char *fmt, ...);
void NORETURN panic_at(const char *file, int line, const char *fmt, ...);

#define ASSERT(cond)                                                          \
    do {                                                                      \
        if (!(cond))                                                          \
            panic_at(__FILE__, __LINE__, "assertion failed: %s", #cond);      \
    } while (0)

#define BUG() panic_at(__FILE__, __LINE__, "BUG")

/* ============================== 版本与状态 =============================== */
#define FYXOS_NAME     "FYXOS"
#define FYXOS_VERSION  "0.1.0-phase1"
#define FYXOS_CODENAME "kernel-core"

void kernel_halt(void) NORETURN;
int  kernel_selftest(void);          /* 返回失败的用例数，0 表示全部通过 */

struct fyx_boot_info;
struct fyx_boot_info *kernel_boot_info(void);
void kmain(struct fyx_boot_info *bi);
void shell_run(void);

#endif /* FYXOS_KERNEL_H */
