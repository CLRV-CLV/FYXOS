/*
 * FYXOS — 用户进程
 * ---------------------------------------------------------------------------
 * 见 include/fyxos/process.h 的头部说明，这里只记录几个实现上的关键点：
 *
 * 1) 用户态现场存在 thread->uregs，由 syscall_entry 在汇编里落盘。fork 靠
 *    拷贝这份现场让子进程"从 fork 返回 0"，exec/init 靠构造这份现场让进程
 *    "从 _start 开始跑"。返回用户态统一走 sysret（arch_return_to_user）。
 *
 * 2) 从 ring 3 陷入 ring 0 时，CPU 会从 TSS 取 RSP0 当作内核栈。所以每次
 *    切换任务都必须更新 g_tss.rsp0，否则中断一来就踩空栈。
 *
 * 3) 每个进程有自己的 PML4，但 PML4 的高 256 项（内核空间）与内核共享同一
 *    份页表树 —— 因此内核代码在任意进程上下文里都能直接跑，不需要切换。
 */
#include <fyxos/process.h>
#include <fyxos/win64.h>
#include <fyxos/pipe.h>
#include <fyxos/sched.h>
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/x86_64.h>
#include <fyxos/syscall.h>
#include <fyxos/vfs.h>

/* 由 isr.S / usermode.S 提供 */
extern void arch_return_to_user(const struct user_regs *r) NORETURN;

static struct process g_procs[MAX_PROCESSES];
static u64            g_next_pid = 1;
static spinlock_t     g_proc_lock = SPINLOCK_INIT("proc");
static struct process *g_init_proc;

void process_init(void)
{
    memset(g_procs, 0, sizeof(g_procs));
    g_next_pid = 1;
    g_init_proc = NULL;
}

struct process *current_process(void)
{
    struct thread *t = current_thread();
    return t ? t->proc : NULL;
}

struct process *process_by_pid(u64 pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (g_procs[i].state != PROC_UNUSED && g_procs[i].pid == pid)
            return &g_procs[i];
    return NULL;
}

struct process *process_create(const char *name)
{
    spin_lock(&g_proc_lock);
    struct process *p = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_procs[i].state == PROC_UNUSED) {
            p = &g_procs[i];
            break;
        }
    }
    if (!p) {
        spin_unlock(&g_proc_lock);
        return NULL;
    }

    memset(p, 0, sizeof(*p));
    p->pid = g_next_pid++;
    p->state = PROC_EMBRYO;
    p->exit_code = 0;
    strncpy(p->name, name ? name : "unnamed", PROC_NAME_MAX - 1);
    list_init(&p->children);
    list_init(&p->child_link);

    spin_unlock(&g_proc_lock);

    if (vmm_create_address_space(&p->as) != 0) {
        p->state = PROC_UNUSED;
        return NULL;
    }
    return p;
}

void process_free(struct process *p)
{
    if (!p || p->state == PROC_UNUSED)
        return;
    pipe_close_all(p);
    if (p->as.pml4)
        vmm_destroy_address_space(&p->as);
    p->state = PROC_UNUSED;
    p->as.pml4 = 0;
}

void process_switch_to(struct process *p)
{
    vmm_switch(p ? p->as.pml4 : vmm_kernel_pml4());
}

/* --------------------------------------------------------------- 地址空间复制 */
/*
 * 遍历父进程 PML4 的低 256 项（用户空间），把每一页复制一份到子进程。
 * TODO: 改成写时复制 —— 两边都标成只读、共享同一批帧，写的时候再复制。
 */
static u32 pte_to_vmm_flags(u64 pte)
{
    u32 f = VMM_USER;
    if (pte & 0x2)                       /* PTE_WRITE */
        f |= VMM_WRITE;
    if (pte & 0x8000000000000000ull)     /* PTE_NX */
        f |= VMM_NOEXEC;
    return f;
}

static int copy_user_space(struct address_space *dst,
                           const struct address_space *src,
                           u64 *out_max)
{
    u64 *pml4 = (u64 *)phys_to_virt(src->pml4);
    u64 max_addr = 0;
    u64 copied = 0;

    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & 0x1))
            continue;
        u64 *pdpt = (u64 *)phys_to_virt(pml4[i] & 0x000FFFFFFFFFF000ull);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & 0x1))
                continue;
            u64 *pd = (u64 *)phys_to_virt(pdpt[j] & 0x000FFFFFFFFFF000ull);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & 0x1))
                    continue;
                if (pd[k] & 0x80)          /* 2 MiB 大页：用户空间目前不用 */
                    return -FYX_EINVAL;
                u64 *pt = (u64 *)phys_to_virt(pd[k] & 0x000FFFFFFFFFF000ull);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & 0x1))
                        continue;

                    u64 virt = ((u64)i << 39) | ((u64)j << 30) |
                               ((u64)k << 21) | ((u64)l << 12);
                    phys_addr_t src_frame = pt[l] & 0x000FFFFFFFFFF000ull;
                    phys_addr_t dst_frame = pmm_alloc_frame();
                    if (dst_frame == PMM_INVALID)
                        return -FYX_ENOMEM;

                    memcpy(phys_to_virt(dst_frame), phys_to_virt(src_frame),
                           PAGE_SIZE);

                    int rc = vmm_map_in(dst, virt, dst_frame, PAGE_SIZE,
                                        pte_to_vmm_flags(pt[l]));
                    if (rc != 0) {
                        pmm_free_frame(dst_frame);
                        return -FYX_ENOMEM;
                    }
                    if (virt + PAGE_SIZE > max_addr)
                        max_addr = virt + PAGE_SIZE;
                    copied++;
                }
            }
        }
    }
    UNUSED(copied);
    if (out_max)
        *out_max = max_addr;
    return 0;
}

/* --------------------------------------------------------------- 返回用户态 */

/* 内核线程的入口：切页表、备好内核栈，然后落到 ring 3，永不返回 */
static void process_thread_entry(void *arg)
{
    struct process *p = (struct process *)arg;
    struct thread *t = current_thread();

    if (!p || !t)
        thread_exit(-1);

    vmm_switch(p->as.pml4);
    gdt_set_kernel_stack((u64)t->stack_base);
    syscall_set_kernel_stack((u64)t->stack_base);
    wrmsr(MSR_FS_BASE, t->fs_base);

    arch_return_to_user(&t->uregs);     /* NORETURN */
}

/* 构造一份"从 _start 开始跑"的现场 */
static void init_user_regs(struct user_regs *r, u64 entry, u64 stack_top)
{
    memset(r, 0, sizeof(*r));
    r->rip    = entry;
    r->rsp    = stack_top;
    r->rflags = 0x202;                  /* 置 IF：用户态允许中断 */
    r->cs     = 0x23;                   /* 用户代码 | RPL3 */
    r->ss     = 0x1b;                   /* 用户数据 | RPL3 */
    r->rax    = 0;
}

/* --------------------------------------------------------------- 系统调用 */

long sys_fork(void)
{
    struct process *parent = current_process();
    struct thread  *pt     = current_thread();
    if (!parent || !pt)
        return -FYX_EINVAL;

    struct process *child = process_create(parent->name);
    if (!child)
        return -FYX_ENOMEM;

    u64 max_addr = 0;
    if (copy_user_space(&child->as, &parent->as, &max_addr) != 0) {
        process_free(child);
        return -FYX_ENOMEM;
    }

    child->entry      = parent->entry;
    child->abi        = parent->abi;
    child->brk        = parent->brk;
    child->brk_base   = parent->brk_base;
    child->brk_mapped = parent->brk_mapped;
    child->mmap_next  = parent->mmap_next;
    child->ustack_top = parent->ustack_top;
    child->parent     = parent;
    pipe_clone_fds(child, parent);

    list_insert_before(&parent->children, &child->child_link);
    parent->nr_children++;

    struct thread *ct = thread_create_embryo(
        child->name, process_thread_entry, child,
        parent->thread ? parent->thread->priority : 5);
    if (!ct) {
        /* 已在上面挂入 children；失败回滚必须同时摘下，否则 wait 会等到
         * 一个状态为 UNUSED、地址空间也已释放的“子进程”。 */
        list_remove(&child->child_link);
        parent->nr_children--;
        process_free(child);
        return -FYX_ENOMEM;
    }

    /* 子进程继承父进程的用户现场，但 fork 在子进程里返回 0 */
    ct->uregs = pt->uregs;
    ct->uregs.rax = 0;
    ct->fs_base = pt->fs_base;
    ct->proc  = child;
    ct->pml4  = child->as.pml4;
    child->thread = ct;
    child->state  = PROC_RUNNABLE;

    kprintf("[proc] fork: %llu -> %llu（%s）\n",
            (unsigned long long)parent->pid,
            (unsigned long long)child->pid, child->name);
    thread_start(ct);
    return (long)child->pid;
}

#define EXEC_STRING_MAX 64

static int load_program_image(const char *name, void **out, size_t *size)
{
    char path[VFS_PATH_MAX];
    if (!name || !*name || strlen(name) >= sizeof(path))
        return -FYX_EINVAL;
    if (strchr(name, '/')) {
        if (snprintf(path, sizeof(path), "%s", name) >= (int)sizeof(path))
            return -FYX_ERANGE;
    } else {
        if (snprintf(path, sizeof(path), "/bin/%s", name) >= (int)sizeof(path))
            return -FYX_ERANGE;
    }
    int rc = vfs_read_file(path, out, size);
    if (rc == 0)
        kprintf("[proc] 从 %s 读取可执行映像（%u 字节）\n",
                path, (unsigned)*size);
    return rc;
}

/* 调用者传入的是当前用户地址空间里的指针；先复制到内核栈，再更换地址空间。
 * 后续安全加固应统一换成带访问检查的 copy_from_user。 */
static int copy_exec_string(char *dst, size_t cap, const char *src)
{
    if (!dst || !src || cap == 0)
        return -FYX_EINVAL;
    size_t n = 0;
    while (n + 1 < cap && src[n]) {
        dst[n] = src[n];
        n++;
    }
    if (src[n] != '\0')
        return -FYX_EINVAL;
    dst[n] = '\0';
    return 0;
}

long sys_exec(const char *name, const char *const argv[],
              const char *const envp[])
{
    struct process *p = current_process();
    struct thread  *t = current_thread();
    if (!p || !t)
        return -FYX_EINVAL;

    /* 这些缓冲区在内核栈上，旧用户空间销毁后仍然有效 */
    char name_copy[VFS_PATH_MAX];
    char arg_store[USER_MAX_ARGS][EXEC_STRING_MAX];
    char env_store[USER_MAX_ARGS][EXEC_STRING_MAX];
    const char *new_argv[USER_MAX_ARGS + 1];
    const char *new_envp[USER_MAX_ARGS + 1];
    int argc = 0, envc = 0;

    if (copy_exec_string(name_copy, sizeof(name_copy), name) != 0 ||
        name_copy[0] == '\0')
        return -FYX_EINVAL;

    while (argv && argv[argc] && argc < USER_MAX_ARGS) {
        if (copy_exec_string(arg_store[argc], EXEC_STRING_MAX,
                             argv[argc]) != 0)
            return -FYX_EINVAL;
        new_argv[argc] = arg_store[argc];
        argc++;
    }
    new_argv[argc] = NULL;
    while (envp && envp[envc] && envc < USER_MAX_ARGS) {
        if (copy_exec_string(env_store[envc], EXEC_STRING_MAX,
                             envp[envc]) != 0)
            return -FYX_EINVAL;
        new_envp[envc] = env_store[envc];
        envc++;
    }
    new_envp[envc] = NULL;

    /* 文件先读入内核缓冲：即使随后切换地址空间，ELF 与参数也都仍有寿命。 */
    void *program = NULL;
    size_t program_size = 0;
    int load_rc = load_program_image(name_copy, &program, &program_size);
    if (load_rc != 0)
        return load_rc;

    /* 在新地址空间里试装载，失败时原进程仍可继续运行 */
    struct address_space new_as = { 0 };
    if (vmm_create_address_space(&new_as) != 0) {
        kfree(program);
        return -FYX_ENOMEM;
    }

    struct process image;
    memset(&image, 0, sizeof(image));
    image.as = new_as;
    image.abi = p->abi;
    strncpy(image.name, p->name, PROC_NAME_MAX - 1);

    if (process_load_elf(&image, program, program_size,
                         new_argv, new_envp) != 0) {
        kfree(program);
        vmm_destroy_address_space(&new_as);
        return -FYX_EINVAL;
    }
    kfree(program);

    /* 成功后才替换旧空间；此刻仍在旧空间执行，切换后再回收旧页表 */
    struct address_space old_as = p->as;
    p->as = new_as;
    p->entry = image.entry;
    p->abi = image.abi;
    p->brk = image.brk;
    p->brk_base = image.brk_base;
    p->brk_mapped = image.brk_mapped;
    p->mmap_next = image.mmap_next;
    p->ustack_top = image.ustack_top;
    strncpy(p->name, name_copy, PROC_NAME_MAX - 1);
    p->state = PROC_RUNNING;
    t->pml4 = p->as.pml4;
    vmm_switch(p->as.pml4);
    vmm_destroy_address_space(&old_as);

    init_user_regs(&t->uregs, p->entry, p->ustack_top);

    /* 直接切回用户态：不再返回到发起 exec 的那条用户指令 */
    gdt_set_kernel_stack((u64)t->stack_base);
    syscall_set_kernel_stack((u64)t->stack_base);
    t->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    arch_return_to_user(&t->uregs);
}

long sys_wait(u64 pid, int *status)
{
    struct process *p = current_process();
    if (!p)
        return -FYX_EINVAL;
    if (p->nr_children == 0)
        return -FYX_EINVAL;

    for (;;) {
        /* 找一个已经变成僵尸的子进程；指定 PID 时同时确认它确实是
         * 当前进程的子进程，避免 wait(别人的 pid) 永久睡眠。 */
        struct process *found = NULL;
        struct process *target = NULL;
        struct list_head *n;
        list_for_each(n, &p->children) {
            struct process *c = container_of(n, struct process, child_link);
            if (pid == 0 || c->pid == pid) {
                target = c;
                if (c->state == PROC_ZOMBIE) {
                    found = c;
                    break;
                }
            }
        }

        if (found) {
            u64 cpid = found->pid;
            if (status)
                *status = found->exit_code;
            list_remove(&found->child_link);
            p->nr_children--;
            process_free(found);
            return (long)cpid;
        }

        /* 没有僵尸子进程：指定 PID 必须是当前子进程 */
        if (pid != 0 && !target)
            return -FYX_EINVAL;

        thread_sleep_ms(10);        /* 让出 CPU，等子进程退出 */
    }
}

long sys_exit(int code)
{
    struct process *p = current_process();
    struct thread  *t = current_thread();
    if (!p)
        thread_exit(code);

    kprintf("[proc] exit: pid=%llu code=%d\n",
            (unsigned long long)p->pid, code);

    p->exit_code = code;
    /* POSIX 语义：退出时立即关闭 fd，不等父进程 wait；
     * 这样最后一个写端退出后，读者可以及时看到 EOF。 */
    pipe_close_all(p);
    p->state = PROC_ZOMBIE;

    /*
     * TODO: 父进程此刻可能正睡在 wait() 里，应当立刻 thread_wake() 它。
     * 阶段 G 先让 wait() 以 10ms 为周期轮询，逻辑正确但会有延迟；
     * 等进程间唤醒关系理顺后改成主动唤醒。
     */

    if (t) {
        /* 回到内核页表：进程地址空间要留到父进程 wait() 之后才回收 */
        t->pml4 = 0;
        vmm_switch(vmm_kernel_pml4());
    }
    thread_exit(code);
}

long sys_getpid(void)
{
    struct process *p = current_process();
    return p ? (long)p->pid : 0;
}

long sys_getppid(void)
{
    struct process *p = current_process();
    return (p && p->parent) ? (long)p->parent->pid : 0;
}

long sys_brk(u64 new_brk)
{
    struct process *p = current_process();
    if (!p)
        return -FYX_EINVAL;
    if (new_brk == 0)
        return (long)p->brk;
    if (new_brk < p->brk_base || new_brk >= USER_MMAP_BASE)
        return p->abi == PROC_ABI_LINUX ? (long)p->brk : -FYX_EINVAL;

    if (new_brk <= p->brk) {
        /* 保留已映射页，逻辑 break 仍可收缩；进程退出时统一回收。 */
        p->brk = new_brk;
        return (long)p->brk;
    }

    u64 mapped_end = ALIGN_UP(new_brk, PAGE_SIZE);
    if (mapped_end > p->brk_mapped) {
        u64 first = p->brk_mapped;
        for (u64 page = first; page < mapped_end; page += PAGE_SIZE) {
            phys_addr_t f = pmm_alloc_frame();
            if (f == PMM_INVALID) {
                if (page > first)
                    vmm_unmap_in(&p->as, first, page - first);
                return p->abi == PROC_ABI_LINUX ? (long)p->brk : -FYX_ENOMEM;
            }
            memset(phys_to_virt(f), 0, PAGE_SIZE);
            if (vmm_map_in(&p->as, page, f, PAGE_SIZE,
                           VMM_USER | VMM_WRITE | VMM_NOEXEC) != 0) {
                pmm_free_frame(f);
                if (page > first)
                    vmm_unmap_in(&p->as, first, page - first);
                return p->abi == PROC_ABI_LINUX ? (long)p->brk : -FYX_ENOMEM;
            }
        }
        p->brk_mapped = mapped_end;
    }
    p->brk = new_brk;
    return (long)p->brk;
}

static u32 linux_prot_to_vmm(u32 prot)
{
    u32 flags = VMM_USER;
    if (prot & 0x2)                 /* PROT_WRITE */
        flags |= VMM_WRITE;
    if (!(prot & 0x4))              /* PROT_EXEC */
        flags |= VMM_NOEXEC;
    return flags;
}

long sys_mmap(u64 addr, size_t len, u32 prot, u32 flags, int fd, u64 offset)
{
    UNUSED(addr);
    UNUSED(fd);
    struct process *p = current_process();
    if (!p || len == 0 || len > USER_AREA_LIMIT || offset != 0 ||
        (prot & ~0x7u) || !(flags & 0x02) || !(flags & 0x20) ||
        (flags & 0x10))
        return -FYX_EINVAL;         /* v1: PRIVATE|ANONYMOUS，拒绝 FIXED */
    size_t bytes = ALIGN_UP(len, PAGE_SIZE);
    u64 base = p->mmap_next ? p->mmap_next : USER_MMAP_BASE;
    if (bytes > USER_AREA_LIMIT - base)
        return -FYX_ENOMEM;

    for (u64 off = 0; off < bytes; off += PAGE_SIZE) {
        phys_addr_t frame = pmm_alloc_frame();
        if (frame == PMM_INVALID) {
            if (off)
                vmm_unmap_in(&p->as, base, off);
            return -FYX_ENOMEM;
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        if (vmm_map_in(&p->as, base + off, frame, PAGE_SIZE,
                       linux_prot_to_vmm(prot)) != 0) {
            pmm_free_frame(frame);
            if (off)
                vmm_unmap_in(&p->as, base, off);
            return -FYX_ENOMEM;
        }
    }
    p->mmap_next = base + bytes;
    return (long)base;
}

long sys_mprotect(u64 addr, size_t len, u32 prot)
{
    struct process *p = current_process();
    if (!p || !IS_ALIGNED(addr, PAGE_SIZE) || len == 0 ||
        (prot & ~0x7u) || addr >= USER_AREA_LIMIT ||
        len > USER_AREA_LIMIT - addr)
        return -FYX_EINVAL;
    return vmm_protect_in(&p->as, addr, ALIGN_UP(len, PAGE_SIZE),
                          linux_prot_to_vmm(prot));
}

long sys_munmap(u64 addr, size_t len)
{
    struct process *p = current_process();
    if (!p || !IS_ALIGNED(addr, PAGE_SIZE) || len == 0 ||
        addr >= USER_AREA_LIMIT || len > USER_AREA_LIMIT - addr)
        return -FYX_EINVAL;
    return vmm_unmap_in(&p->as, addr, ALIGN_UP(len, PAGE_SIZE));
}

/* --------------------------------------------------------------- 启动 init */

void process_start_init(void)
{
    const char *argv[] = { "init", NULL };
    const char *envp[] = { "FYXOS=1", NULL };

    struct process *p = process_create("init");
    if (!p)
        panic("process: cannot create init");
    g_init_proc = p;
    p->parent = NULL;

    void *program = NULL;
    size_t size = 0;
    int rc = load_program_image("/bin/init", &program, &size);
    if (rc != 0 || size == 0)
        panic("process: 无法从 FYXFS 读取 /bin/init（%d）", rc);
    if (process_load_elf(p, program, size, argv, envp) != 0) {
        kfree(program);
        panic("process: 装载 /bin/init 失败");
    }
    kfree(program);

    struct thread *t = thread_create_embryo("init", process_thread_entry, p, 5);
    if (!t)
        panic("process: 无法为 init 建立线程");

    init_user_regs(&t->uregs, p->entry, p->ustack_top);
    t->proc = p;
    t->pml4 = p->as.pml4;
    p->thread = t;
    p->state = PROC_RUNNABLE;

    kprintf("[proc] init 就绪：pid=%llu entry=0x%llx\n",
            (unsigned long long)p->pid, (unsigned long long)p->entry);
    thread_start(t);
}

/* 回收没有父进程等待的退出映像（包括完成自检后退出的 init）。 */
static void process_reap_orphans(void)
{
    for (u32 i = 0; i < MAX_PROCESSES; i++) {
        struct process *p = &g_procs[i];
        if (p->state != PROC_ZOMBIE || p->parent)
            continue;
        if (p->thread)
            p->thread->proc = NULL;
        if (g_init_proc == p)
            g_init_proc = NULL;
        process_free(p);
    }
}

int process_run_wait_abi(const char *name, enum process_abi abi, int *exit_code)
{
    if (!name || !*name || (abi != PROC_ABI_FYXOS && abi != PROC_ABI_LINUX &&
                            abi != PROC_ABI_WIN64))
        return -FYX_EINVAL;
    process_reap_orphans();

    struct process *p = process_create(name);
    if (!p)
        return -FYX_ENOMEM;
    p->parent = NULL;
    p->abi = abi;

    void *program = NULL;
    size_t size = 0;
    int rc = load_program_image(name, &program, &size);
    if (rc != 0 || size == 0) {
        process_free(p);
        return rc != 0 ? rc : -FYX_EINVAL;
    }
    const char *argv[] = { name, NULL };
    const char *envp[] = { "FYXOS=1", NULL };
    rc = abi == PROC_ABI_WIN64 ? process_load_pe(p, program, size) :
        process_load_elf(p, program, size, argv, envp);
    kfree(program);
    if (rc != 0) {
        process_free(p);
        return rc;
    }

    struct thread *t = thread_create_embryo(name, process_thread_entry, p, 5);
    if (!t) {
        process_free(p);
        return -FYX_ENOMEM;
    }
    init_user_regs(&t->uregs, p->entry, p->ustack_top);
    t->proc = p;
    t->pml4 = p->as.pml4;
    p->thread = t;
    p->state = PROC_RUNNABLE;
    thread_start(t);

    while (p->state != PROC_ZOMBIE)
        thread_sleep_ms(1);
    if (exit_code)
        *exit_code = p->exit_code;
    t->proc = NULL;
    process_free(p);
    return FYX_OK;
}

int process_run_wait(const char *name, int *exit_code)
{
    return process_run_wait_abi(name, PROC_ABI_FYXOS, exit_code);
}
