/* FYXOS — 固定 ImageBase 的 Win64 PE32+ 装载器与真实导入解析 */
#include <fyxos/process.h>
#include <fyxos/win64.h>
#include <fyxos/mm.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>

#define PE_MACHINE_AMD64 0x8664u
#define PE_MAGIC_64      0x020bu
#define PE_SCN_EXECUTE   0x20000000u
#define PE_SCN_READ      0x40000000u
#define PE_SCN_WRITE     0x80000000u
#define PE_MAX_SECTIONS  16u
#define PE_MAX_IMPORTS   64u

struct pe_section {
    u32 vsize, rva, raw_size, raw_ptr, chars;
};

struct pe_image {
    const u8 *file;
    size_t size;
    u64 base;
    u32 size_image, headers_size, entry;
    u16 count;
    struct pe_section s[PE_MAX_SECTIONS];
};

static u16 rd16(const void *p)
{
    const u8 *b = p;
    return (u16)b[0] | ((u16)b[1] << 8);
}

static u32 rd32(const void *p)
{
    const u8 *b = p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) |
           ((u32)b[3] << 24);
}

static u64 rd64(const void *p)
{
    return (u64)rd32(p) | ((u64)rd32((const u8 *)p + 4) << 32);
}

static int file_range(size_t size, u64 off, u64 len)
{
    return off <= size && len <= size - off;
}

static int parse_headers(struct pe_image *pe, const void *image, size_t size)
{
    memset(pe, 0, sizeof(*pe));
    pe->file = image;
    pe->size = size;
    if (size < 0x40 || rd16(image) != 0x5a4d)
        return -FYX_EINVAL;
    u32 po = rd32((const u8 *)image + 0x3c);
    if (!file_range(size, po, 24) ||
        memcmp((const u8 *)image + po, "PE\0\0", 4) != 0)
        return -FYX_EINVAL;
    const u8 *coff = pe->file + po + 4;
    if (rd16(coff) != PE_MACHINE_AMD64)
        return -FYX_EINVAL;
    pe->count = rd16(coff + 2);
    u16 opt_size = rd16(coff + 16);
    u64 opt = po + 24;
    if (pe->count == 0 || pe->count > PE_MAX_SECTIONS || opt_size < 120 ||
        !file_range(size, opt, opt_size) || rd16(pe->file + opt) != PE_MAGIC_64)
        return -FYX_EINVAL;
    const u8 *o = pe->file + opt;
    pe->entry = rd32(o + 16);
    pe->base = rd64(o + 24);
    u32 section_align = rd32(o + 32);
    u32 file_align = rd32(o + 36);
    pe->size_image = rd32(o + 56);
    pe->headers_size = rd32(o + 60);
    u64 sh = opt + opt_size;
    if (section_align < PAGE_SIZE || (section_align & (section_align - 1)) ||
        file_align < 512 || (file_align & (file_align - 1)) ||
        !file_range(size, sh, (u64)pe->count * 40) ||
        pe->headers_size < sh + (u64)pe->count * 40 ||
        pe->headers_size > size || (pe->size_image & (section_align - 1)) ||
        pe->base < PAGE_SIZE || pe->base >= USER_AREA_LIMIT ||
        pe->size_image < PAGE_SIZE || pe->size_image > 64u * 1024u * 1024u ||
        pe->size_image > USER_AREA_LIMIT - pe->base)
        return -FYX_EINVAL;
    bool entry_ok = false;
    u64 header_pages = ALIGN_UP(pe->headers_size, PAGE_SIZE);
    for (u16 i = 0; i < pe->count; i++) {
        const u8 *h = pe->file + sh + i * 40;
        struct pe_section *s = &pe->s[i];
        s->vsize = rd32(h + 8);
        s->rva = rd32(h + 12);
        s->raw_size = rd32(h + 16);
        s->raw_ptr = rd32(h + 20);
        s->chars = rd32(h + 36);
        if (!s->vsize)
            s->vsize = s->raw_size;
        if (!s->vsize || s->rva < section_align || s->rva < header_pages ||
            (s->rva & (section_align - 1)) ||
            (s->raw_size && (s->raw_ptr & (file_align - 1))) ||
            !file_range(size, s->raw_ptr, s->raw_size) ||
            s->rva >= pe->size_image ||
            s->vsize > pe->size_image - s->rva ||
            !(s->chars & PE_SCN_READ))
            return -FYX_EINVAL;
        if (pe->entry >= s->rva && pe->entry - s->rva < s->vsize) {
            if (!(s->chars & PE_SCN_EXECUTE))
                return -FYX_EINVAL;
            entry_ok = true;
        }
    }
    return entry_ok ? 0 : -FYX_EINVAL;
}

/* RVA 到文件数据的映射同时用于导入解析和边界验证。写导入槽时不需要
 * 文件后备，但仍要求目标完整位于某个已声明节的 VirtualSize 内。 */
static void *rva_file(const struct pe_image *pe, u32 rva, u32 len, int write)
{
    for (u16 i = 0; i < pe->count; i++) {
        const struct pe_section *s = &pe->s[i];
        if (rva < s->rva)
            continue;
        u64 d = (u64)rva - s->rva;
        if (d > s->vsize || len > s->vsize - d)
            continue;
        if (write)
            return (void *)(uintptr_t)(pe->base + rva);
        if (d > s->raw_size || len > s->raw_size - d ||
            !file_range(pe->size, s->raw_ptr + d, len))
            return NULL;
        return (void *)(pe->file + s->raw_ptr + d);
    }
    return NULL;
}

static const char *rva_cstr(const struct pe_image *pe, u32 rva)
{
    if (rva > 0xffffffffu - 64u)
        return NULL;
    const char *s = rva_file(pe, rva, 1, 0);
    if (!s)
        return NULL;
    for (u32 i = 0; i < 64; i++) {
        const char *p = rva_file(pe, rva + i, 1, 0);
        if (!p)
            return NULL;
        if (*p == '\0')
            return s;
    }
    return NULL;
}

static int map_anon(struct address_space *as, u64 virt, u64 bytes, u32 flags)
{
    for (u64 off = 0; off < bytes; off += PAGE_SIZE) {
        phys_addr_t frame = pmm_alloc_frame();
        if (frame == PMM_INVALID)
            return -FYX_ENOMEM;
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        if (vmm_map_in(as, virt + off, frame, PAGE_SIZE, flags) != 0) {
            pmm_free_frame(frame);
            return -FYX_ENOMEM;
        }
    }
    return 0;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* 每个导入目标都是用户态可执行的间接调用桩。桩把 Win64 寄存器参数与
 * 栈上第 5/6 参数转换为内核 syscall ABI；用户永远无法直接调用内核地址。 */
static void make_stub(u8 *s, u32 nr)
{
    static const u8 prefix[] = {
        0x57,                               /* push rdi */
        0x56,                               /* push rsi */
        0x48, 0x89, 0xcf,                   /* mov rdi,rcx */
        0x48, 0x89, 0xd6,                   /* mov rsi,rdx */
        0x4c, 0x89, 0xc2,                   /* mov rdx,r8 */
        0x4d, 0x89, 0xca,                   /* mov r10,r9 */
        0x4c, 0x8b, 0x44, 0x24, 0x38,       /* mov r8,[rsp+38] */
        0x4c, 0x8b, 0x4c, 0x24, 0x40,       /* mov r9,[rsp+40] */
        0xb8                                /* mov eax,imm32 */
    };
    static const u8 suffix[] = { 0x0f, 0x05, 0x5e, 0x5f, 0xc3 };
    memcpy(s, prefix, sizeof(prefix));
    put32(s + sizeof(prefix), nr);
    memcpy(s + sizeof(prefix) + 4, suffix, sizeof(suffix));
}

static int map_stubs(struct address_space *as)
{
    phys_addr_t frame = pmm_alloc_frame();
    if (frame == PMM_INVALID)
        return -FYX_ENOMEM;
    u8 *page = phys_to_virt(frame);
    memset(page, 0, PAGE_SIZE);
    make_stub(page + 0 * WIN32_STUB_STRIDE, SYS_WIN32_GET_STD_HANDLE);
    make_stub(page + 1 * WIN32_STUB_STRIDE, SYS_WIN32_WRITE_FILE);
    make_stub(page + 2 * WIN32_STUB_STRIDE, SYS_WIN32_EXIT_PROCESS);
    if (vmm_map_in(as, WIN32_STUB_BASE, frame, PAGE_SIZE, VMM_USER) != 0) {
        pmm_free_frame(frame);
        return -FYX_ENOMEM;
    }
    return 0;
}

static int import_target(const char *name, u64 *target)
{
    if (strcmp(name, "GetStdHandle") == 0)
        *target = WIN32_STUB_BASE;
    else if (strcmp(name, "WriteFile") == 0)
        *target = WIN32_STUB_BASE + WIN32_STUB_STRIDE;
    else if (strcmp(name, "ExitProcess") == 0)
        *target = WIN32_STUB_BASE + 2 * WIN32_STUB_STRIDE;
    else
        return -FYX_ENOSYS;
    return 0;
}

static int resolve_imports(struct process *p, const struct pe_image *pe,
                           u32 import_rva)
{
    if (!import_rva)
        return 0;
    int rc = map_stubs(&p->as);
    if (rc)
        return rc;
    phys_addr_t saved = vmm_current_pml4();
    vmm_switch(p->as.pml4);
    rc = 0;
    for (u32 d = 0; d < PE_MAX_SECTIONS; d++) {
        const u8 *desc = rva_file(pe, import_rva + d * 20, 20, 0);
        if (!desc) {
            rc = -FYX_EINVAL;
            break;
        }
        u32 lookup = rd32(desc), name_rva = rd32(desc + 12);
        u32 iat = rd32(desc + 16);
        if (!lookup && !name_rva && !iat)
            break;
        if (rd32(desc + 8) || !name_rva || !iat) {
            rc = -FYX_ENOSYS;
            break;
        }
        const char *dll = rva_cstr(pe, name_rva);
        if (!dll || (strcmp(dll, "kernel32.dll") != 0 &&
                     strcmp(dll, "KERNEL32.DLL") != 0)) {
            kprintf("[pe] 不支持的 DLL 导入：%s\n", dll ? dll : "<bad-rva>");
            rc = -FYX_ENOSYS;
            break;
        }
        if (!lookup)
            lookup = iat;
        bool terminated = false;
        for (u32 n = 0; n < PE_MAX_IMPORTS; n++) {
            const u8 *tp = rva_file(pe, lookup + n * 8, 8, 0);
            u64 *slot = rva_file(pe, iat + n * 8, 8, 1);
            if (!tp || !slot) {
                rc = -FYX_EINVAL;
                break;
            }
            u64 thunk = rd64(tp);
            if (!thunk) {
                terminated = true;
                break;
            }
            if (thunk & (1ull << 63)) {
                kprintf("[pe] 暂不支持 ordinal 导入\n");
                rc = -FYX_ENOSYS;
                break;
            }
            const char *name = rva_cstr(pe, (u32)thunk + 2);
            u64 target = 0;
            if (!name || import_target(name, &target) != 0) {
                kprintf("[pe] 不支持的 kernel32 导入：%s\n",
                        name ? name : "<bad-rva>");
                rc = -FYX_ENOSYS;
                break;
            }
            *slot = target;
        }
        if (!rc && !terminated)
            rc = -FYX_EINVAL;
        if (rc)
            break;
        if (d + 1 == PE_MAX_SECTIONS)
            rc = -FYX_ENOSYS;
    }
    vmm_switch(saved);
    return rc;
}

int process_load_pe(struct process *p, const void *image, size_t size)
{
    struct pe_image pe;
    int rc = parse_headers(&pe, image, size);
    if (rc)
        return rc;
    p->abi = PROC_ABI_WIN64;
    u32 import_rva = 0;
    u32 po = rd32(pe.file + 0x3c);
    u16 opt_size = rd16(pe.file + po + 20);
    const u8 *o = pe.file + po + 24;
    if (opt_size >= 128 && rd32(o + 108) > 1)
        import_rva = rd32(o + 120);

    /* Windows 把 PE 头作为 ImageBase 的只读/NX 页映射；程序可读取 MZ/PE。 */
    for (u64 off = 0; off < ALIGN_UP(pe.headers_size, PAGE_SIZE); off += PAGE_SIZE) {
        phys_addr_t frame = pmm_alloc_frame();
        if (frame == PMM_INVALID)
            return -FYX_ENOMEM;
        u8 *dst = phys_to_virt(frame);
        memset(dst, 0, PAGE_SIZE);
        u64 n = pe.headers_size - off;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        memcpy(dst, pe.file + off, n);
        if (vmm_map_in(&p->as, pe.base + off, frame, PAGE_SIZE,
                       VMM_USER | VMM_NOEXEC) != 0) {
            pmm_free_frame(frame);
            return -FYX_ENOMEM;
        }
    }

    for (u16 i = 0; i < pe.count; i++) {
        const struct pe_section *s = &pe.s[i];
        u64 start = pe.base + s->rva;
        u64 bytes = ALIGN_UP(s->vsize, PAGE_SIZE);
        u32 flags = VMM_USER;
        if (s->chars & PE_SCN_WRITE)
            flags |= VMM_WRITE;
        if (!(s->chars & PE_SCN_EXECUTE))
            flags |= VMM_NOEXEC;
        for (u64 off = 0; off < bytes; off += PAGE_SIZE) {
            phys_addr_t frame = pmm_alloc_frame();
            if (frame == PMM_INVALID)
                return -FYX_ENOMEM;
            u8 *dst = phys_to_virt(frame);
            memset(dst, 0, PAGE_SIZE);
            u64 raw = s->raw_size < s->vsize ? s->raw_size : s->vsize;
            if (off < raw) {
                u64 n = raw - off;
                if (n > PAGE_SIZE)
                    n = PAGE_SIZE;
                memcpy(dst, pe.file + s->raw_ptr + off, n);
            }
            if (vmm_map_in(&p->as, start + off, frame, PAGE_SIZE, flags) != 0) {
                pmm_free_frame(frame);
                return -FYX_ENOMEM;
            }
        }
    }
    rc = map_anon(&p->as, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                  VMM_USER | VMM_WRITE | VMM_NOEXEC);
    if (rc)
        return rc;
    rc = resolve_imports(p, &pe, import_rva);
    if (rc)
        return rc;

    /* 临时可写的可执行节在 IAT 修补后降为 RX；其余数据节始终 NX。 */
    for (u16 i = 0; i < pe.count; i++) {
        const struct pe_section *s = &pe.s[i];
        if ((s->chars & (PE_SCN_EXECUTE | PE_SCN_WRITE)) ==
            (PE_SCN_EXECUTE | PE_SCN_WRITE)) {
            rc = vmm_protect_in(&p->as, pe.base + s->rva,
                                ALIGN_UP(s->vsize, PAGE_SIZE), VMM_USER);
            if (rc)
                return rc;
        }
    }
    p->entry = pe.base + pe.entry;
    p->ustack_top = USER_STACK_TOP;
    p->brk = ALIGN_UP(pe.base + pe.size_image, PAGE_SIZE);
    p->brk_base = p->brk;
    p->brk_mapped = p->brk;
    p->mmap_next = USER_MMAP_BASE;
    kprintf("[proc] 装载 %s ABI=Win64：entry=0x%llx image=0x%llx+0x%x 栈顶=0x%llx\n",
            p->name, (unsigned long long)p->entry,
            (unsigned long long)pe.base, pe.size_image,
            (unsigned long long)p->ustack_top);
    return 0;
}
