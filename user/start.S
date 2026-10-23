/*
 * FYXOS — 用户程序入口 _start
 *
 * SysV AMD64 ABI：进程入口时 RSP 指向 argc，其后依次是 argv[]（NULL 结尾）、
 * envp[]（NULL 结尾）。这与内核 elf_load.c 里的栈布置严格对应。
 */
    .intel_syntax noprefix
    .section .text, "ax"
    .globl _start
    .extern main

_start:
    mov     rdi, [rsp]                  /* argc */
    lea     rsi, [rsp + 8]              /* argv */
    lea     rdx, [rsp + 8 + rdi * 8 + 8]/* envp = argv 之后跳过 NULL */

    /* 栈按 ABI 要求保持 16 字节对齐（进入 _start 时 RSP 已对齐，call 会再压 8） */
    and     rsp, -16
    sub     rsp, 8
    call    main

    /* main 返回就把返回值当退出码 */
    mov     rdi, rax
    mov     rax, 0x1013                 /* SYS_EXIT */
    syscall
    hlt                                 /* 正常情况下到不了 */

.section .note.GNU-stack,"",@progbits
