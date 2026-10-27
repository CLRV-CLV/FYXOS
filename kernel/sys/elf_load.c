/*
 * FYXOS — 用户态 ELF64 装载器
 * ---------------------------------------------------------------------------
 * 与 boot/stage2_32.c 里那个装载器是两回事：
 *   - stage2 装载器跑在实模式/32 位，把内核搬到 p_paddr（物理地址），顺序拷贝。
 *   - 这个跑在内核态，为**进程自己的地址空间**建立映射到 p_vaddr，逐段设置
 *     W^X 权限，并布置 SysV ABI 要求的用户栈（argc/argv/envp/auxv）。
 *
 * 只支持静态链接的 ET_EXEC（非 PIE）。动态链接与 PIE 等有了文件系统与
 * 动态链接器之后再做。
 */
#include <fyxos/elf.h>
#include <fyxos/process.h>
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>
#include <fyxos/x86_64.h>

/* 用户栈顶布置（SysV AMD64 ABI §3.4.1），从低地址往高地址看：
 *
 *   RSP -> +-------------------------+
 *          | argc                    |
 *          +-------------------------+
 *          | argv[0] .. argv[n-1]    |
 *          | NULL                    |
 *          +-------------------------+
 *          | envp[0] .. envp[m-1]    |
 *          | NULL                    |
 *          +-------------------------+
 *          | auxv[]  (AT_NULL 结尾)  |
 *          +-------------------------+
 *          | 参数串 / 环境串          |  <- 紧贴 USER_STACK_TOP 之下
 *          +-------------------------+
 */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

#define AUXV_WITH_PHDR 6       /* PAGESZ/PHDR/PHENT/PHNUM/ENTRY/NULL */
#define AUXV_MINIMAL   3       /* PAGESZ/ENTRY/NULL */
#define AUXV_LINUX_MAX 15      /* 上述项 + Linux 身份/随机数/执行路径 */

static int validate_ehdr(const struct elf64_ehdr *eh, size_t size)
{
    if (size < sizeof(*eh))
        return -FYX_EINVAL;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0)
        return -FYX_EINVAL;
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB)
        return -FYX_EINVAL;
    if (eh->e_machine != EM_X86_64)
        return -FYX_EINVAL;
    /* 阶段 G 只做固定地址的静态 ET_EXEC；ET_DYN 需要重定位/动态链接器 */
    if (eh->e_type != ET_EXEC)
        return -FYX_EINVAL;
    if (eh->e_phoff == 0 || eh->e_phnum == 0 ||
        eh->e_phentsize < sizeof(struct elf64_phdr))
        return -FYX_EINVAL;
    if (eh->e_phoff + (u64)eh->e_phnum * eh->e_phentsize > size)
        return -FYX_EINVAL;
    return 0;
}

/* 往进程页表里映射 count 个匿名页（清零），返回 0 成功 */
static int map_anon(struct address_space *as, u64 virt, size_t count, u32 flags)
{
    for (size_t i = 0; i < count; i++) {
        phys_addr_t frame = pmm_alloc_frame();
        if (frame == PMM_INVALID)
            return -FYX_ENOMEM;
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        if (vmm_map_in(as, virt + i * PAGE_SIZE, frame, PAGE_SIZE, flags) != 0) {
            pmm_free_frame(frame);
            return -FYX_ENOMEM;
        }
    }
    return 0;
}

int process_load_elf(struct process *p, const void *image, size_t size,
                     const char *const argv[], const char *const envp[])
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    int rc = validate_ehdr(eh, size);
    if (rc != 0)
        return rc;

    /* GCC/binutils 的 Linux 静态程序通常带 ELFOSABI_GNU。System-V OSABI=0
     * 在两种系统间都可能出现，只有显式 run linux 才选择 Linux ABI。 */
    if (p->abi == PROC_ABI_FYXOS && eh->e_ident[7] == ELFOSABI_GNU)
        p->abi = PROC_ABI_LINUX;

    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)((const u8 *)image + eh->e_phoff);

    u64 max_addr = 0;
    u64 phdr_addr = 0;
    u64 phdr_bytes = (u64)eh->e_phnum * eh->e_phentsize;

    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            kprintf("[elf] 拒绝动态 ELF：PT_INTERP/动态链接器尚未实现\n");
            return -FYX_ENOSYS;
        }
    }

    /* ---------------- 1. 装载 PT_LOAD ---------------- */
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (ph[i].p_memsz == 0)
            continue;
        if (ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_offset > size ||
            ph[i].p_filesz > size - ph[i].p_offset)
            return -FYX_EINVAL;

        if (ph[i].p_vaddr >= USER_AREA_LIMIT ||
            ph[i].p_memsz > USER_AREA_LIMIT - ph[i].p_vaddr)
            return -FYX_EINVAL;
        u64 vstart = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE);
        u64 vend   = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE);

        /* 用户空间边界：不能碰到内核，也不能是非规范地址 */
        if (vstart < PAGE_SIZE || vend > USER_AREA_LIMIT)
            return -FYX_EINVAL;

        u32 flags = VMM_USER;                       /* 用户态可访问 */
        if (ph[i].p_flags & PF_W)
            flags |= VMM_WRITE;
        if (!(ph[i].p_flags & PF_X))
            flags |= VMM_NOEXEC;                    /* W^X：不可执行段一律 NX */

        u64 file_end = ph[i].p_vaddr + ph[i].p_filesz;

        /* 只有当程序头表本身也被装入用户空间时，AT_PHDR 才有意义 */
        if (eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff + phdr_bytes <= ph[i].p_offset + ph[i].p_filesz)
            phdr_addr = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);

        for (u64 page = vstart; page < vend; page += PAGE_SIZE) {
            phys_addr_t frame = pmm_alloc_frame();
            if (frame == PMM_INVALID)
                return -FYX_ENOMEM;

            u8 *dst = (u8 *)phys_to_virt(frame);
            memset(dst, 0, PAGE_SIZE);

            /* 属于 filesz 的字节从镜像拷；其余（.bss）留零 */
            u64 copy_start = page > ph[i].p_vaddr ? page : ph[i].p_vaddr;
            u64 copy_end   = (page + PAGE_SIZE) < file_end
                                 ? (page + PAGE_SIZE) : file_end;
            if (copy_start < copy_end) {
                u64 file_off = ph[i].p_offset + (copy_start - ph[i].p_vaddr);
                u64 n = copy_end - copy_start;
                if (file_off + n > size)
                    n = size - file_off;
                memcpy(dst + (copy_start - page), (const u8 *)image + file_off, n);
            }

            rc = vmm_map_in(&p->as, page, frame, PAGE_SIZE, flags);
            if (rc != 0) {
                pmm_free_frame(frame);
                return -FYX_ENOMEM;
            }
        }

        if (vend > max_addr)
            max_addr = vend;
    }

    if (max_addr == 0)
        return -FYX_EINVAL;

    p->entry = eh->e_entry;
    p->brk = ALIGN_UP(max_addr, PAGE_SIZE);
    if (p->abi == PROC_ABI_FYXOS && p->brk < USER_HEAP_BASE)
        p->brk = USER_HEAP_BASE;
    p->brk_base = p->brk;
    p->brk_mapped = p->brk;
    p->mmap_next = USER_MMAP_BASE;

    /* ---------------- 2. 用户栈 ---------------- */
    u64 stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    rc = map_anon(&p->as, stack_base, USER_STACK_SIZE / PAGE_SIZE,
                  VMM_USER | VMM_WRITE | VMM_NOEXEC);
    if (rc != 0)
        return rc;

    /* ---------------- 3. 数出 argc / envc ---------------- */
    int argc = 0, envc = 0;
    while (argv && argv[argc] && argc < USER_MAX_ARGS)
        argc++;
    while (envp && envp[envc] && envc < USER_MAX_ARGS)
        envc++;

    /* ---------------- 4. 算好布局（自顶向下） ---------------- */
    u64 arg_ptrs[USER_MAX_ARGS];
    u64 env_ptrs[USER_MAX_ARGS];

    u64 cur = USER_STACK_TOP;
    for (int i = 0; i < envc; i++) {          /* 环境串在最上面 */
        cur -= strlen(envp[i]) + 1;
        env_ptrs[i] = cur;
    }
    for (int i = 0; i < argc; i++) {
        cur -= strlen(argv[i]) + 1;
        arg_ptrs[i] = cur;
    }
    u64 random_addr = 0;
    if (p->abi == PROC_ABI_LINUX) {
        cur -= 16;
        random_addr = cur;
    }
    cur = ALIGN_DOWN(cur, 16);

    int auxv_pairs = p->abi == PROC_ABI_LINUX ? AUXV_LINUX_MAX :
        (phdr_addr ? AUXV_WITH_PHDR : AUXV_MINIMAL);

    /* argc/argv/envp/auxv 必须无空洞连续。对齐 padding 只能放在 argc 之前，
     * 否则 glibc 会把 envp NULL 后的空洞误认为 AT_NULL，丢失 AT_RANDOM。 */
    u64 vector_bytes = 8 + ((u64)argc + 1) * 8 + ((u64)envc + 1) * 8 +
                       (u64)auxv_pairs * 16;
    cur = ALIGN_DOWN(cur - vector_bytes, 16);
    u64 sp_final = cur;
    u64 auxv_base = sp_final + 8 + ((u64)argc + 1) * 8 +
                    ((u64)envc + 1) * 8;

    /* ---------------- 5. 切到进程页表把内容写进去 ---------------- */
    phys_addr_t saved_cr3 = vmm_current_pml4();
    vmm_switch(p->as.pml4);

    for (int i = 0; i < envc; i++)
        memcpy((void *)env_ptrs[i], envp[i], strlen(envp[i]) + 1);
    for (int i = 0; i < argc; i++)
        memcpy((void *)arg_ptrs[i], argv[i], strlen(argv[i]) + 1);

    if (random_addr) {
        /* 仅用于早期 glibc stack guard；不是密码学随机数服务。 */
        u64 seed = rdtsc() ^ ((u64)p->pid << 32) ^ eh->e_entry;
        for (int i = 0; i < 16; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            ((u8 *)(uintptr_t)random_addr)[i] = (u8)seed;
        }
    }

    u64 *av = (u64 *)auxv_base;
#define AUXV_PUT(key, value) do { av[0] = (key); av[1] = (value); av += 2; } while (0)
    AUXV_PUT(AT_PAGESZ, PAGE_SIZE);
    if (phdr_addr) {
        AUXV_PUT(AT_PHDR, phdr_addr);
        AUXV_PUT(AT_PHENT, eh->e_phentsize);
        AUXV_PUT(AT_PHNUM, eh->e_phnum);
    }
    AUXV_PUT(AT_ENTRY, eh->e_entry);
    if (p->abi == PROC_ABI_LINUX) {
        AUXV_PUT(AT_HWCAP, 0);
        AUXV_PUT(AT_CLKTCK, 100);
        AUXV_PUT(AT_UID, 0);
        AUXV_PUT(AT_EUID, 0);
        AUXV_PUT(AT_GID, 0);
        AUXV_PUT(AT_EGID, 0);
        AUXV_PUT(AT_SECURE, 0);
        AUXV_PUT(AT_RANDOM, random_addr);
        AUXV_PUT(AT_EXECFN, argc ? arg_ptrs[0] : 0);
    }
    AUXV_PUT(AT_NULL, 0);
#undef AUXV_PUT

    u64 *q = (u64 *)sp_final;
    *q++ = (u64)argc;
    for (int i = 0; i < argc; i++)
        *q++ = arg_ptrs[i];
    *q++ = 0;
    for (int i = 0; i < envc; i++)
        *q++ = env_ptrs[i];
    *q++ = 0;

    vmm_switch(saved_cr3);

    p->ustack_top = sp_final;

    kprintf("[proc] 装载 %s ABI=%s：entry=0x%llx brk=0x%llx 栈顶=0x%llx argc=%d\n",
            p->name, p->abi == PROC_ABI_LINUX ? "Linux-x86_64" : "FYXOS",
            (unsigned long long)p->entry,
            (unsigned long long)p->brk,
            (unsigned long long)p->ustack_top, argc);
    return 0;
}
