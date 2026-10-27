/*
 * FYXOS — 用户进程（阶段 G）
 * ---------------------------------------------------------------------------
 * 本阶段的目标是把"用户态"立起来：进程拥有**独立的用户地址空间**，通过
 * `syscall` 从 ring 3 进入内核，能 fork / exec / wait / exit。
 *
 * 刻意简化的地方（后续阶段补齐，都在下面标注了 TODO）：
 *   - 一进程一线程：进程与线程是 1:1，`struct process` 里直接持有线程指针。
 *     多线程（pthread 语义）等有了用户态线程库再做。
 *   - fork 用**全量复制**而不是写时复制：逻辑简单、不会错。等页故障处理
 *     完善后改成 CoW，届时复制一次只需标记只读 + 共享帧。
 *   - exec 已通过 VFS 按路径读取 ELF；内嵌 blob 只作为首启安装种子。
 *     用户指针目前仍主要在信任的最小 init 中使用，完整 copy_from_user
 *     边界检查是后续安全加固项。
 */
#ifndef FYXOS_PROCESS_H
#define FYXOS_PROCESS_H

#include <fyxos/types.h>
#include <fyxos/mm.h>
#include <fyxos/sync.h>

/* ---------------------------------------------------------------- 地址布局 */
/*
 * 用户空间只占 PML4 的第 0 项（0 ~ 512 GiB），内核占高半核。
 * 布局刻意留了大片空隙，便于日后加 ASLR 与 mmap 区域。
 */
#define USER_CODE_BASE   0x0000000000400000ull   /* 非 PIE 程序默认链接地址 */
#define USER_HEAP_BASE   0x0000000010000000ull   /* brk 起点 */
#define USER_STACK_TOP   0x000000007ffff000ull   /* 栈顶（不含） */
#define USER_STACK_SIZE  (64ull * 1024ull)       /* 用户栈大小 */
#define USER_MMAP_BASE   0x0000000020000000ull   /* Linux 匿名 mmap 起点 */
#define USER_AREA_LIMIT  0x0000800000000000ull   /* 用户空间上界（非规范地址起） */

#define MAX_PROCESSES    32
#define PROC_NAME_MAX    32
#define USER_MAX_ARGS    16
#define PROC_MAX_FDS     16

struct fyx_pipe;
struct fyx_file;
struct fyx_socket;

enum fyx_fd_type {
    FYX_FD_NONE = 0,
    FYX_FD_PIPE = 1,
    FYX_FD_FILE = 2,
    FYX_FD_SOCKET = 3,
};

/* fd 是进程表项；文件偏移、管道端点与 socket 生命周期由被引用对象维护。 */
struct fyx_fd {
    struct fyx_pipe *pipe;
    struct fyx_file *file;
    struct fyx_socket *socket;
    u8               type;        /* enum fyx_fd_type */
    u8               flags;       /* FYX_FD_READ / FYX_FD_WRITE */
};

enum process_abi {
    PROC_ABI_FYXOS = 0,
    PROC_ABI_LINUX = 1,
    PROC_ABI_WIN64 = 2,
};

enum process_state {
    PROC_UNUSED = 0,
    PROC_EMBRYO,        /* 刚建好、还没装载镜像 */
    PROC_RUNNABLE,
    PROC_RUNNING,
    PROC_SLEEPING,      /* wait() 中等子进程 */
    PROC_ZOMBIE,
};

struct process {
    u64                pid;
    struct process    *parent;
    enum process_state state;
    int                exit_code;

    struct address_space as;       /* 用户地址空间（PML4 物理地址） */
    u64                entry;      /* 程序入口（虚拟地址） */
    enum process_abi   abi;        /* FYXOS、Linux x86_64 或 Win64/Win32 ABI */
    u64                brk;        /* 逻辑程序断点（堆顶） */
    u64                brk_base;   /* 允许收缩到的初始堆顶 */
    u64                brk_mapped; /* 已映射堆页的末端 */
    u64                mmap_next;  /* 匿名 mmap 的单调分配游标 */
    u64                ustack_top;

    struct thread     *thread;     /* TODO: 一进程一线程，日后改成线程链表 */
    char               name[PROC_NAME_MAX];

    struct list_head   child_link; /* 挂在父进程的 children 链表上 */
    struct list_head   children;
    u32                nr_children;

    struct fyx_fd      fds[PROC_MAX_FDS];
};

/* ---------------------------------------------------------------- 接口 */
void process_init(void);

struct process *process_create(const char *name);
void            process_free(struct process *p);
struct process *process_by_pid(u64 pid);
struct process *current_process(void);
void            process_switch_to(struct process *p);

/* 把内存中的 ELF64 镜像装进进程地址空间，并布置好 argc/argv/envp 栈 */
int  process_load_elf(struct process *p, const void *image, size_t size,
                      const char *const argv[], const char *const envp[]);

/* 让某个线程开始执行用户程序：切到进程页表，iretq 落到 ring 3。
 * 由 thread 的入口函数调用，永不返回。 */
void NORETURN process_enter_userspace(struct process *p);

/* ------------------------------------------------------- 系统调用实现 */
long sys_fork(void);
long sys_exec(const char *name, const char *const argv[],
              const char *const envp[]);
long sys_wait(u64 pid, int *status);
long sys_exit(int code);
long sys_getpid(void);
long sys_getppid(void);
long sys_brk(u64 new_brk);
long sys_mmap(u64 addr, size_t len, u32 prot, u32 flags, int fd, u64 offset);
long sys_mprotect(u64 addr, size_t len, u32 prot);
long sys_munmap(u64 addr, size_t len);

/* 内嵌用户程序镜像（tools/mkimage 之外，由 .incbin 随内核提供） */
extern const u8 g_init_image_start[];
extern const u8 g_init_image_end[];
extern const u8 g_gcc_demo_image_start[];
extern const u8 g_gcc_demo_image_end[];
extern const u8 g_deb_demo_image_start[];
extern const u8 g_deb_demo_image_end[];
extern const u8 g_win64_demo_image_start[];
extern const u8 g_win64_demo_image_end[];
extern const u8 g_deb_plain_image_start[];
extern const u8 g_deb_plain_image_end[];
extern const u8 g_deb_bad_image_start[];
extern const u8 g_deb_bad_image_end[];
extern const u8 g_win64_bad_image_start[];
extern const u8 g_win64_bad_image_end[];

void process_start_init(void);   /* 由 kmain 调用：装载并运行第一个用户进程 */

/* 内核 Shell/IDE 启动一个磁盘 ELF，等待退出并回收；返回装载/调度错误。 */
int  process_run_wait(const char *name, int *exit_code);
int  process_run_wait_abi(const char *name, enum process_abi abi, int *exit_code);

#endif /* FYXOS_PROCESS_H */
