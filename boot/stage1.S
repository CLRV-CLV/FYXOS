/*
 * FYXOS stage1 — MBR 引导扇区 (512 字节, 16 位实模式)
 * ---------------------------------------------------------------------------
 * 职责：把 stage2 从磁盘读进内存并跳转过去。不依赖任何既有 OS/引导器。
 *
 * 进入时：DL = BIOS 引导驱动器号，CS:IP 可能是 07C0:0000 或 0000:7C00
 * 离开时：jmp 0000:8000，DL 保持引导驱动器号，SI = DAP 地址
 *
 * 磁盘布局（LBA）：
 *   0        stage1 (本文件)
 *   1..64    stage2（32 KiB，末尾 16 字节为 stage2 描述符）
 *   65..     内核 ELF64 映像
 *   2048..   FYXOS 系统分区（见 MBR 分区表）
 *
 * 优先使用 BIOS 扩展读（INT 13h AH=42h，LBA）；失败则回退到
 * INT 13h AH=02h（CHS），以兼容不支持 LBA 的老机器。
 */
    .intel_syntax noprefix
    .code16
    .section .text.start, "ax"
    .globl _start

/* --- MBR 头内的引导描述符偏移（由 tools/mkimage.py 填充） ----------------- */
#define MBR_DESC_MAGIC      0x7DB0   /* "FYXB"                       */
#define MBR_STAGE2_SECTORS  0x7DB4   /* u16: stage2 占用扇区数        */
#define MBR_KERNEL_LBA      0x7DB6   /* u32: 内核映像起始 LBA         */
#define MBR_KERNEL_SECTORS  0x7DBA   /* u32: 内核映像扇区数           */
#define MBR_MAGIC_VALUE     0x42585946  /* "FYXB" */

#define DAP_ADDR            0x0500   /* Disk Address Packet 暂存区    */
#define STAGE2_SEGMENT      0x0000
#define STAGE2_OFFSET       0x8000

_start:
    cli
    jmp     0x0000, offset normalize_cs     /* 统一 CS 基址为 0 */

normalize_cs:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00                      /* 栈向下生长到 0x7000 区 */
    sti

    mov     [0x500C], dl                    /* boot_info.boot_drive */
    mov     byte ptr [boot_drive], dl

    /* 校验引导描述符，防止镜像未经过 mkimage 生成 */
    mov     eax, [MBR_DESC_MAGIC]
    cmp     eax, MBR_MAGIC_VALUE
    jne     bad_image

    mov     si, offset msg_banner
    call    print_string

    /* ---------------- LBA 扩展读 ---------------------------------------- */
    call    load_stage2_lba
    jc      try_chs
    jmp     verify_stage2

try_chs:
    call    load_stage2_chs
    jc      disk_error

verify_stage2:
    /* stage2 末尾描述符的魔数 "FYX2" */
    mov     eax, [0xFFF0]
    cmp     eax, 0x32585946
    jne     bad_image

    mov     si, offset msg_ok
    call    print_string

    /* ------------------------------------------------------------------
     * 跳转至 stage2
     *    DL  = 引导驱动器号
     *    DS=ES=SS=0
     * ------------------------------------------------------------------ */
    mov     dl, byte ptr [boot_drive]
    jmp     STAGE2_SEGMENT, STAGE2_OFFSET

/* =========================== 子程序 ===================================== */

/* print_string: DS:SI 指向的 NUL 结尾字符串 -> BIOS teletype */
print_string:
    lodsb
    or      al, al
    jz      print_done
    mov     ah, 0x0E
    mov     bh, 0x00
    mov     bl, 0x07
    int     0x10
    jmp     print_string
print_done:
    ret

/* load_stage2_lba: 用 INT 13h AH=42h 读取 stage2；CF=1 表示失败 */
load_stage2_lba:
    mov     di, DAP_ADDR
    mov     byte ptr [di + 0x00], 0x10      /* DAP 大小 */
    mov     byte ptr [di + 0x01], 0x00      /* 保留 */
    mov     ax, [MBR_STAGE2_SECTORS]
    mov     [di + 0x02], ax                 /* 扇区数 */
    mov     word ptr [di + 0x04], STAGE2_OFFSET
    mov     word ptr [di + 0x06], STAGE2_SEGMENT
    mov     dword ptr [di + 0x08], 1        /* LBA 低 32 位（stage2 起始 LBA=1） */
    mov     dword ptr [di + 0x0C], 0        /* LBA 高 32 位 */
    mov     ah, 0x42
    mov     dl, byte ptr [boot_drive]
    mov     si, DAP_ADDR
    int     0x13
    ret

/* load_stage2_chs: CHS 回退路径。先取磁盘几何参数，再逐扇区读取 */
load_stage2_chs:
    mov     ah, 0x08
    mov     dl, byte ptr [boot_drive]
    xor     di, di
    mov     es, di
    int     0x13
    jc      chs_failed
    /* CL[5:0] = 每磁道扇区数, DH = 最大磁头号, CH = 最大柱面号低 8 位 */
    mov     cl, cl
    and     cl, 0x3F
    mov     byte ptr [spt], cl
    mov     byte ptr [heads], dh
    inc     byte ptr [heads]
    test    byte ptr [spt], 0xFF            /* 保证不为 0 */
    jnz     chs_have_geometry
chs_failed:
    stc
    ret
chs_have_geometry:
    mov     ax, [MBR_STAGE2_SECTORS]
    mov     word ptr [chs_remaining], ax
    mov     dword ptr [chs_lba], 1
    mov     word ptr [chs_offset], STAGE2_OFFSET

chs_next:
    cmp     word ptr [chs_remaining], 0
    je      chs_done
    call    lba_to_chs                      /* -> CH=柱面, DH=磁头, CL=扇区 */
    mov     ax, 0x0201                      /* AH=02 读扇区, AL=1 */
    mov     bx, word ptr [chs_offset]
    mov     dl, byte ptr [boot_drive]
    int     0x13
    jc      chs_failed
    add     word ptr [chs_offset], 512
    inc     dword ptr [chs_lba]
    dec     word ptr [chs_remaining]
    jmp     chs_next
chs_done:
    clc
    ret

/* lba_to_chs: 把 dword [chs_lba] 转换为 CH/DH/CL */
lba_to_chs:
    mov     eax, dword ptr [chs_lba]
    xor     edx, edx
    movzx   ecx, byte ptr [spt]
    div     ecx                             /* EAX=磁道号, EDX=扇区内偏移 */
    inc     dl                              /* 扇区号从 1 开始 */
    mov     byte ptr [chs_sector], dl
    xor     edx, edx
    movzx   ecx, byte ptr [heads]
    div     ecx                             /* EAX=柱面, EDX=磁头 */
    mov     byte ptr [chs_head], dl
    mov     byte ptr [chs_cylinder], al
    mov     cl, byte ptr [chs_sector]
    mov     dh, byte ptr [chs_head]
    mov     ch, byte ptr [chs_cylinder]
    ret

disk_error:
    mov     si, offset msg_disk_error
    call    print_string
    jmp     halt_forever

bad_image:
    mov     si, offset msg_bad_image
    call    print_string
    jmp     halt_forever

halt_forever:
    cli
    hlt
    jmp     halt_forever

/* ------------------------------ 数据 ------------------------------------ */
boot_drive:     .byte 0
spt:            .byte 63
heads:          .byte 16
chs_sector:     .byte 0
chs_head:       .byte 0
chs_cylinder:   .byte 0
chs_lba:        .long 1
chs_remaining:  .word 0
chs_offset:     .word 0

msg_banner:     .asciz "FYXOS stage1\r\n"
msg_ok:         .asciz "stage2 ok\r\n"
msg_disk_error: .asciz "FYXOS: disk read error\r\n"
msg_bad_image:  .asciz "FYXOS: bad image\r\n"
