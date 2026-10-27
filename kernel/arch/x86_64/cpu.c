/*
 * FYXOS — CPU 特性探测（CPUID）
 *
 * 注意：某些模拟器（包括 fyxemu 用的 Unicorn）对 CPUID 的支持不完整，
 * 可能返回全 0。此时按“x86-64 基线”保守兜底，保证内核仍能跑起来。
 */
#include <fyxos/x86_64.h>
#include <fyxos/lib.h>
#include <fyxos/kernel.h>

struct cpu_features g_cpu;

struct cpuid_result cpuid(u32 leaf, u32 subleaf)
{
    struct cpuid_result r = { 0, 0, 0, 0 };
    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
    return r;
}

static void cpuid_vendor(struct cpuid_result *r, char out[13])
{
    u32 regs[3] = { r->ebx, r->edx, r->ecx };
    memcpy(out, regs, 12);
    out[12] = '\0';
}

/*
 * 品牌串兜底。
 *
 * 有些虚拟机/模拟器（fyxemu 用的 Unicorn 就是）不实现 0x80000002..4
 * 这组扩展叶子，此时品牌串是空的。真机上这种情况几乎见不到，但空串会让
 * uname / [cpu] 打印出 "unknown"，很难看。这里改用 CPUID.1 已经拿到的
 * vendor + family/model/stepping 拼一个可辨识的名字。
 */
static void cpu_brand_fallback(char out[49])
{
    snprintf(out, 49, "%s family %u model %u stepping %u",
             g_cpu.vendor[0] ? g_cpu.vendor : "unknown",
             (unsigned)g_cpu.family,
             (unsigned)g_cpu.model,
             (unsigned)g_cpu.stepping);
}

static void cpuid_brand(char out[49])
{
    char buf[49];
    memset(buf, 0, sizeof(buf));
    for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        struct cpuid_result r = cpuid(leaf, 0);
        u32 *p = (u32 *)&buf[(leaf - 0x80000002) * 16];
        p[0] = r.eax; p[1] = r.ebx; p[2] = r.ecx; p[3] = r.edx;
    }
    /* 去掉前导空格 */
    const char *s = buf;
    while (*s == ' ')
        s++;
    strncpy(out, s, 48);
    out[48] = '\0';
}

static void cpu_enable_sse2(void)
{
    /* 长模式用户态基线包含 SSE2；内核自身仍用 -mno-sse* 编译。 */
    write_cr0((read_cr0() | 0x2) & ~0xcull); /* MP=1，EM=TS=0 */
    write_cr4(read_cr4() | 0x600);           /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("fninit");
    u32 mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
}

void cpu_init(void)
{
    memset(&g_cpu, 0, sizeof(g_cpu));

    struct cpuid_result r0 = cpuid(0, 0);
    if (r0.eax == 0) {
        /* CPUID 不可用或被模拟器屏蔽：按 x86-64 基线兜底 */
        strncpy(g_cpu.vendor, "unknown", sizeof(g_cpu.vendor) - 1);
        strncpy(g_cpu.brand, "generic x86-64 (CPUID unavailable)", 48);
        g_cpu.has_msr = true;
        g_cpu.has_tsc = true;
        g_cpu.has_syscall = true;
        g_cpu.has_apic = true;
        g_cpu.has_sse2 = true;
        g_cpu.max_physical_bits = 36;
        g_cpu.max_virtual_bits = 48;
        cpu_enable_sse2();
        return;
    }

    cpuid_vendor(&r0, g_cpu.vendor);

    struct cpuid_result r1 = cpuid(1, 0);
    g_cpu.has_msr     = !!(r1.edx & (1u << 5));
    g_cpu.has_apic    = !!(r1.edx & (1u << 9));
    g_cpu.has_tsc     = !!(r1.edx & (1u << 4));
    g_cpu.has_syscall = !!(r1.edx & (1u << 11));      /* 实际是 SYSCALL，见下方 */
    g_cpu.has_sse2    = !!(r1.edx & (1u << 26));
    g_cpu.has_pge     = !!(r1.edx & (1u << 13));
    g_cpu.family      = ((r1.eax >> 8) & 0xF) + ((r1.eax >> 20) & 0xFF);
    g_cpu.model       = ((r1.eax >> 4) & 0xF) | ((r1.eax >> 12) & 0xF0);
    g_cpu.stepping    = r1.eax & 0xF;

    /* SYSCALL/SYSRET 实际由扩展功能位（8000_0001.EDX[11]）指示 */
    struct cpuid_result r8 = cpuid(0x80000001, 0);
    g_cpu.has_syscall = !!(r8.edx & (1u << 11));
    g_cpu.has_nx      = !!(r8.edx & (1u << 20));
    g_cpu.has_1gb_pages = !!(r8.edx & (1u << 26));
    g_cpu.has_rdtscp  = !!(r8.edx & (1u << 27));

    struct cpuid_result r7 = cpuid(7, 0);
    g_cpu.has_avx = !!(r7.ebx & (1u << 5));

    struct cpuid_result r8008 = cpuid(0x80000008, 0);
    g_cpu.max_physical_bits = (u8)(r8008.eax & 0xFF);
    g_cpu.max_virtual_bits  = (u8)((r8008.eax >> 8) & 0xFF);
    if (g_cpu.max_physical_bits == 0)
        g_cpu.max_physical_bits = 36;
    if (g_cpu.max_virtual_bits == 0)
        g_cpu.max_virtual_bits = 48;

    if (r0.eax >= 0x80000004)
        cpuid_brand(g_cpu.brand);

    /* 品牌串为空（扩展叶子不可用）时退回到 CPUID.1 的描述 */
    if (!g_cpu.brand[0] || g_cpu.brand[0] == ' ')
        cpu_brand_fallback(g_cpu.brand);

    /* 启用 NX：CPU 支持时必须打开 EFER.NXE，否则 vmm 写出的带 NX 位的
     * 页表项会被判为"保留位被置位"，任何访问该页的指令都触发 #PF。 */
    if (g_cpu.has_nx)
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | MSR_EFER_NXE);
    if (g_cpu.has_sse2)
        cpu_enable_sse2();
}
