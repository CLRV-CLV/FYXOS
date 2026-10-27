/*
 * FYXOS stage2 (16 位部分)
 * ---------------------------------------------------------------------------
 *   1. 开启 A20 地址线
 *   2. 通过 INT 15h EAX=E820h 探测物理内存，填入 boot_info
 *   3. 用 BIOS INT 13h 把内核 ELF64 映像读入 0x10000（实模式可达的暂存区）
 *   4. 切到 32 位保护模式，交给 stage2_32.c
 *
 * 本文件与 stage2_32.c / stage2_32.S 一起链接为 0x8000 处的 flat binary。
 * .text16 段的第一个字节就是入口。
 */
    .intel_syntax noprefix

/* boot_info 相关偏移（与 include/fyxos/boot_info.h 保持一致） */
#define BOOT_INFO       0x5000
#define BI_MAGIC_OFF    0x00
#define BI_VERSION_OFF  0x04
#define BI_E820_COUNT   0x08
#define BI_BOOT_DRIVE   0x0C
#define BI_E820_TABLE   0x50
#define E820_MAX        64
#define E820_ENTRY_SIZE 24

#define SCRATCH_KERNEL_ENTRY 0x0600
#define SCRATCH_STACK_TOP    0x0608
#define SCRATCH_BOOT_INFO    0x0610

#define DAP_ADDR        0x0500
/*
 * 内核映像暂存区用**32 位线性地址**跟踪，每次读盘时再拆成 seg:off。
 *
 * 原来的写法是固定段 + 16 位偏移，并且用 `add dword ptr [krn_off], eax`
 * 推进：由于数据段里 krn_seg 声明在 krn_off 之前，这条 dword 加法的
 * 高 16 位其实落进了 krn_count，段永远不前进、偏移绕回 0。结果第 3 块
 * （32 KiB 之后）把第 1 块覆盖了，暂存区开头只剩最后写入的补零数据，
 * stage2_32 一看不是 ELF 就直接 FATAL。
 */
#define KERNEL_BUF_LIN  0x10000     /* == KERNEL_STAGE_ADDR，实模式可达 */

    .section .text16, "ax"
    .code16
    .globl _stage2_start

_stage2_start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    mov     si, offset s2_msg
    call    print_string

    /* boot_info 头部：magic/version，drive 已由 stage1 写入 */
    mov     dword ptr [BOOT_INFO + BI_MAGIC_OFF], 0x31585946
    mov     dword ptr [BOOT_INFO + BI_VERSION_OFF], 1

    call    enable_a20
    call    detect_memory
    call    load_kernel

    mov     si, offset s2_loaded
    call    print_string

    /* ---------------- 进入 32 位保护模式 -------------------------------- */
    cli
    lgdt    [stage2_gdtr]
    mov     eax, cr0
    or      eax, 0x1
    mov     cr0, eax
    jmp     0x08, offset pm_entry     /* 刷新 CS，进入 32 位代码段 */

/* ========================== 16 位子程序 ================================= */

print_string:
    lodsb
    or      al, al
    jz      ps_done
    mov     ah, 0x0E
    mov     bh, 0x00
    mov     bl, 0x07
    int     0x10
    jmp     print_string
ps_done:
    ret

/* enable_a20: 依次尝试 BIOS / Fast A20 / 8042 键盘控制器 */
enable_a20:
    mov     ax, 0x2401
    int     0x15
    jnc     a20_done
    in      al, 0x92
    test    al, 0x02
    jnz     a20_done
    or      al, 0x02
    out     0x92, al
a20_done:
    ret

/* detect_memory: E820 -> boot_info.e820[]，并统计可用内存 */
detect_memory:
    xor     eax, eax
    mov     dword ptr [BOOT_INFO + BI_E820_COUNT], eax
    mov     edi, BOOT_INFO + BI_E820_TABLE
    xor     ebx, ebx
    mov     dword ptr [e820_total], 0
    mov     dword ptr [e820_total + 4], 0

e820_loop:
    mov     eax, 0x0000E820
    mov     edx, 0x534D4150      /* "SMAP" */
    mov     ecx, E820_ENTRY_SIZE
    int     0x15
    jc      e820_finished
    cmp     eax, 0x534D4150
    jne     e820_finished

    /* 跳过长度为 0 的项 */
    mov     eax, dword ptr [edi + 8]
    or      eax, dword ptr [edi + 12]
    jz      e820_next

    mov     eax, dword ptr [edi + 16]
    cmp     eax, 1               /* type == usable? */
    jne     e820_count_only
    mov     eax, dword ptr [edi + 8]
    add     dword ptr [e820_total], eax
    mov     eax, dword ptr [edi + 12]
    adc     dword ptr [e820_total + 4], eax

e820_count_only:
    inc     dword ptr [BOOT_INFO + BI_E820_COUNT]
    add     edi, E820_ENTRY_SIZE

e820_next:
    mov     eax, dword ptr [BOOT_INFO + BI_E820_COUNT]
    cmp     eax, E820_MAX
    jae     e820_finished
    test    ebx, ebx
    jz      e820_finished
    jmp     e820_loop

e820_finished:
    mov     eax, dword ptr [e820_total]
    mov     dword ptr [BOOT_INFO + 0x30], eax
    mov     eax, dword ptr [e820_total + 4]
    mov     dword ptr [BOOT_INFO + 0x34], eax
    ret

/*
 * load_kernel: 读内核映像到 0x10000（KERNEL_STAGE_ADDR）。
 * 每次最多 64 扇区（32 KiB），规避 BIOS 单次传输上限。
 */
load_kernel:
    mov     eax, [0xFFF4]        /* kernel_lba  (stage2 描述符) */
    mov     dword ptr [krn_lba], eax
    mov     eax, [0xFFF8]        /* kernel_sectors */
    mov     dword ptr [krn_left], eax
    mov     dword ptr [krn_linear], KERNEL_BUF_LIN

krn_loop:
    cmp     dword ptr [krn_left], 0
    je      krn_done
    mov     eax, dword ptr [krn_left]
    cmp     eax, 64
    jbe     krn_size_ok
    mov     eax, 64
krn_size_ok:
    mov     word ptr [krn_count], ax

    mov     di, DAP_ADDR
    mov     byte ptr [di + 0x00], 0x10
    mov     byte ptr [di + 0x01], 0x00
    mov     [di + 0x02], ax
    /* 线性地址 -> seg:off（off 只取低 4 位，段承担其余部分，物理上连续） */
    mov     ebx, dword ptr [krn_linear]
    mov     ecx, ebx
    shr     ebx, 4                       /* seg = linear >> 4   */
    and     ecx, 0x0F                    /* off = linear & 0xF  */
    mov     [di + 0x04], cx
    mov     [di + 0x06], bx
    mov     eax, dword ptr [krn_lba]
    mov     dword ptr [di + 0x08], eax
    mov     dword ptr [di + 0x0C], 0
    mov     ah, 0x42
    mov     dl, byte ptr [BOOT_INFO + BI_BOOT_DRIVE]
    mov     si, DAP_ADDR
    int     0x13
    jc      krn_error

    xor     eax, eax
    mov     ax, word ptr [krn_count]
    add     dword ptr [krn_lba], eax
    sub     dword ptr [krn_left], eax
    shl     eax, 9                       /* 扇区数 * 512 */
    add     dword ptr [krn_linear], eax
    jmp     krn_loop

krn_error:
    mov     si, offset s2_krn_error
    call    print_string
    cli
    hlt
krn_done:
    ret

/* --------------------------- 16 位数据 ---------------------------------- */
e820_total:     .quad 0
krn_lba:        .long 0
krn_left:       .long 0
krn_linear:     .long 0
krn_count:      .word 0

s2_msg:         .asciz "FYXOS stage2\r\n"
s2_loaded:      .asciz "kernel loaded\r\n"
s2_krn_error:   .asciz "FYXOS: kernel read error\r\n"

/* ====================== GDT（16/32/64 位共用） ========================== */
    .align 8
    .globl stage2_gdt
stage2_gdt:
    .quad 0x0000000000000000     /* 0x00 null        */
    .quad 0x00CF9A000000FFFF     /* 0x08 32-bit code */
    .quad 0x00CF92000000FFFF     /* 0x10 32-bit data */
    .quad 0x00AF9A000000FFFF     /* 0x18 64-bit code */
    .quad 0x00AF92000000FFFF     /* 0x20 64-bit data */
stage2_gdt_end:

stage2_gdtr:
    .word stage2_gdt_end - stage2_gdt - 1
    .long stage2_gdt

/* ====================== 32 位保护模式入口 =============================== */
    .section .text32, "ax"
    .code32
    .globl pm_entry

pm_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    /* BIOS 已用完：32 位栈放到高端 0x1F0000..0x1FEFFF，绝不能再与
     * 0x10000 起步、可增长的内核 ELF 暂存区重叠。栈顶不参与段复制。 */
    mov     esp, 0x1FF000

    /* 清空 .bss（C 代码依赖） */
    mov     edi, offset __bss_start
    mov     ecx, offset __bss_end
    sub     ecx, edi
    shr     ecx, 2
    xor     eax, eax
    rep     stosd

    call    stage2_main

    /* stage2_main 不应返回 */
    cli
pm_halt:
    hlt
    jmp     pm_halt

/* ====================== 64 位长模式跳板 ================================= */
/*
 * 由 stage2_32.S 的 enter_long_mode() 通过 far jmp 进入。
 * 为避免在 elf_i386 目标里产生 64 位重定位，此处不使用任何符号，
 * 全部通过固定的低地址暂存单元（见 SCRATCH_*）与 C 侧交换数据。
 */
    .section .text64, "ax"
    .code64
    .globl lm_trampoline

lm_trampoline:
    mov     ax, 0x20
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax

    mov     rsp, [SCRATCH_STACK_TOP]
    mov     rdi, [SCRATCH_BOOT_INFO]
    xor     rbp, rbp
    mov     rax, [SCRATCH_KERNEL_ENTRY]
    jmp     rax
