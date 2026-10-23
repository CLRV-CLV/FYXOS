/*
 * FYXOS — 内核自检
 * ---------------------------------------------------------------------------
 * 在 kmain 早期（调度器启动之前）运行，也可以在 Shell 里用 test 命令随时重跑。
 * 返回失败的用例数；0 表示全部通过。
 *
 * 自检遵循“不依赖中断”的原则：调度器尚未启动时也能跑，因此不测定时器。
 * 定时器与抢占调度由 sched 演示线程 + Shell 的 ps 命令验证。
 */
#include <fyxos/kernel.h>
#include <fyxos/lib.h>
#include <fyxos/mm.h>
#include <fyxos/syscall.h>
#include <fyxos/x86_64.h>
#include <fyxos/fyxfs.h>

#define SELFTEST(name)  static int t_##name(void)
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            kprintf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/* ---------------------------------------------------------------- 字符串 */
SELFTEST(string)
{
    CHECK(strlen("FYXOS") == 5);
    CHECK(strlen("") == 0);
    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strncmp("abcd", "abce", 3) == 0);

    char buf[32];
    memset(buf, 0x5A, sizeof(buf));
    CHECK(buf[0] == 0x5A && buf[31] == 0x5A);

    char src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    char dst[8];
    memcpy(dst, src, 8);
    CHECK(memcmp(dst, src, 8) == 0);
    CHECK(memcmp(dst, src, 7) == 0);

    /* 重叠拷贝必须用 memmove */
    char ov[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    memmove(ov + 2, ov, 6);
    CHECK(ov[0] == 1 && ov[2] == 1 && ov[3] == 2 && ov[7] == 6);
    return 0;
}

/* ---------------------------------------------------------------- printf */
SELFTEST(printf)
{
    char b[64];

    ksnprintf(b, sizeof(b), "%s", "FYXOS");
    CHECK(strcmp(b, "FYXOS") == 0);

    ksnprintf(b, sizeof(b), "%d", -42);
    CHECK(strcmp(b, "-42") == 0);

    ksnprintf(b, sizeof(b), "%u", 42u);
    CHECK(strcmp(b, "42") == 0);

    ksnprintf(b, sizeof(b), "0x%x", 0xdeadbeefu);
    CHECK(strcmp(b, "0xdeadbeef") == 0);

    ksnprintf(b, sizeof(b), "0x%08X", 0xABCu);
    CHECK(strcmp(b, "0x00000ABC") == 0);

    ksnprintf(b, sizeof(b), "%llu", 123456789012345ull);
    CHECK(strcmp(b, "123456789012345") == 0);

    ksnprintf(b, sizeof(b), "%c%c", 'O', 'K');
    CHECK(strcmp(b, "OK") == 0);

    ksnprintf(b, sizeof(b), "%5d|", 7);
    CHECK(strcmp(b, "    7|") == 0);

    /* 截断：缓冲区只有 4 字节时应写入 3 个字符 + NUL */
    ksnprintf(b, 4, "abcdef");
    CHECK(strlen(b) == 3 && strcmp(b, "abc") == 0);
    return 0;
}

/* ------------------------------------------------------------------ 堆 */
SELFTEST(heap)
{
    struct heap_stats before, after;
    heap_stats(&before);

    void *a = kmalloc(128);
    void *b = kmalloc(256);
    void *c = kmalloc(512);
    CHECK(a && b && c);
    CHECK(a != b && b != c && a != c);

    memset(a, 0x11, 128);
    memset(b, 0x22, 256);
    memset(c, 0x33, 512);
    CHECK(*(u8 *)a == 0x11 && *(u8 *)b == 0x22 && *(u8 *)c == 0x33);

    kfree(b);
    /* 释放后重新申请同样大小，应当复用刚释放的块（合并逻辑生效） */
    void *b2 = kmalloc(256);
    CHECK(b2 != NULL);

    kfree(a);
    kfree(c);
    kfree(b2);

    heap_stats(&after);
    /* 全部释放后，已用量不应高于测试开始时的水平（允许块头开销的微小差异） */
    CHECK(after.used <= before.used + 64);
    CHECK(after.free_count > before.free_count);
    return 0;
}

/* ------------------------------------------------------- 物理帧分配器 */
SELFTEST(pmm)
{
    phys_addr_t frames[16];
    for (int i = 0; i < 16; i++) {
        frames[i] = pmm_alloc_frame();
        CHECK(frames[i] != PMM_INVALID);
        CHECK((frames[i] & (PAGE_SIZE - 1)) == 0);      /* 必须页对齐 */
        for (int j = 0; j < i; j++)
            CHECK(frames[j] != frames[i]);              /* 不能重复分配 */
    }

    void *v = phys_to_virt(frames[0]);
    volatile u32 *p = (volatile u32 *)v;
    *p = 0xC0FFEEu;
    CHECK(*p == 0xC0FFEEu);                             /* 写入后可读回 */

    struct pmm_stats s1, s2;
    pmm_stats(&s1);
    for (int i = 0; i < 16; i++)
        pmm_free_frame(frames[i]);
    pmm_stats(&s2);
    CHECK(s2.free_frames > s1.free_frames);
    return 0;
}

/* --------------------------------------------------------- 虚拟内存 */
SELFTEST(vmm)
{
    phys_addr_t page = pmm_alloc_frame();
    CHECK(page != PMM_INVALID);

    u64 virt = 0xFFFFFFFFA0000000ull;      /* 内核堆上方的一块空闲虚拟地址 */
    CHECK(vmm_map(virt, page, PAGE_SIZE, VMM_WRITE | VMM_NOEXEC) == 0);

    phys_addr_t got = 0;
    CHECK(vmm_translate(virt, &got) == 0);
    CHECK(got == page);

    volatile u64 *slot = (volatile u64 *)virt;
    slot[0] = 0xFEEDFACECAFEBEEFull;
    CHECK(slot[0] == 0xFEEDFACECAFEBEEFull);
    /* 同一虚拟页的尾部也要可写 */
    volatile u8 *tail = (volatile u8 *)(virt + PAGE_SIZE - 1);
    *tail = 0x7F;
    CHECK(*tail == 0x7F);

    CHECK(vmm_unmap(virt, PAGE_SIZE) == 0);
    CHECK(vmm_translate(virt, &got) != 0);

    pmm_free_frame(page);
    return 0;
}

/* --------------------------------------------------------- 系统调用 ABI */
SELFTEST(syscall)
{
    long r = syscall_dispatch(SYS_DEBUG_ECHO, 0x12345678, 0, 0, 0, 0, 0);
    CHECK(r == 0x12345678);

    r = syscall_dispatch(SYS_GET_VERSION, 0, 0, 0, 0, 0, 0);
    CHECK(r == (long)(uintptr_t)FYXOS_VERSION);

    /* 未实现的调用必须返回 -ENOSYS，而不是崩溃 */
    r = syscall_dispatch(0x7FFFFFFF, 0, 0, 0, 0, 0, 0);
    CHECK(r == -FYX_ENOSYS);
    return 0;
}

/* ------------------------------------------------------------ CPU 特性 */
SELFTEST(cpu)
{
    /* 能跑到这里说明已经在 64 位长模式；CPUID 至少不能崩 */
    struct cpuid_result r = cpuid(0, 0);
    (void)r;
    CHECK(read_cr3() != 0);                 /* 分页必须已经开启 */
    CHECK((read_cr0() & 0x80000000) != 0);  /* CR0.PG */
    CHECK((read_cr4() & 0x20) != 0);        /* CR4.PAE */
    CHECK((rdmsr(MSR_EFER) & 0x400) != 0);  /* EFER.LMA：长模式已激活 */
    return 0;
}

/* -------------------------------------------------------------- 控制台 */
SELFTEST(console)
{
    u32 before = g_klog.head;
    const char *s = "selftest-console";
    console_write(s, strlen(s));
    CHECK(g_klog.head == before + (u32)strlen(s));
    CHECK(g_klog.buf[(g_klog.head - 1) % FYX_LOG_SIZE] == 'e');
    return 0;
}

/* --------------------------------------------------------- 存储校验算法 */
SELFTEST(crc32)
{
    /* CRC-32/ISO-HDLC 标准向量，防止根块/文件校验实现静默漂移。 */
    CHECK(fyx_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(fyx_crc32("", 0) == 0);
    CHECK(fyx_crc32("FYXFS", 5) != fyx_crc32("FYXFT", 5));
    return 0;
}

/* ============================== 测试 runner ============================= */
struct selftest_case {
    const char *name;
    int (*fn)(void);
};

static const struct selftest_case g_cases[] = {
    { "string",  t_string  },
    { "printf",  t_printf  },
    { "cpu",     t_cpu     },
    { "pmm",     t_pmm     },
    { "vmm",     t_vmm     },
    { "heap",    t_heap    },
    { "syscall", t_syscall },
    { "console", t_console },
    { "crc32",   t_crc32   },
};

int kernel_selftest(void)
{
    int failures = 0;
    for (size_t i = 0; i < ARRAY_SIZE(g_cases); i++) {
        kprintf("  [%s] ...", g_cases[i].name);
        int r = g_cases[i].fn();
        if (r == 0)
            kprintf(" ok\n");
        else {
            kprintf(" FAILED\n");
            failures++;
        }
    }
    return failures;
}
