/*
 * FYXOS — 基础类型
 * 内核与引导器共用，不依赖任何宿主头文件（freestanding）。
 */
#ifndef FYXOS_TYPES_H
#define FYXOS_TYPES_H

typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef unsigned long      size_t;
typedef signed long        ssize_t;
typedef unsigned long      uintptr_t;
typedef signed long        intptr_t;
typedef unsigned long      phys_addr_t;
typedef unsigned long      virt_addr_t;

typedef int                bool;
#define true  1
#define false 0

#define NULL ((void *)0)

/* 常用宏 ------------------------------------------------------------------ */
#define ARRAY_SIZE(a)        (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)       (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)     ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)     (((x) & ((a) - 1)) == 0)
#define MIN(a, b)            ((a) < (b) ? (a) : (b))
#define MAX(a, b)            ((a) > (b) ? (a) : (b))
#define offsetof(t, m)       __builtin_offsetof(t, m)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))

#define PAGE_SIZE   0x1000ull
#define PAGE_MASK   (~(PAGE_SIZE - 1))
#define BIGPAGE_SIZE 0x200000ull

/* 编译器提示 -------------------------------------------------------------- */
#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define UNUSED(x)   ((void)(x))
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define barrier()   __asm__ volatile("" ::: "memory")

/* 通用错误码（系统调用与内核接口共用，返回值取负） */
#define FYX_OK      0
#define FYX_EINVAL  1
#define FYX_ENOMEM  2
#define FYX_ENOSYS  3
#define FYX_EAGAIN  4
#define FYX_EFAULT  5

#endif /* FYXOS_TYPES_H */
