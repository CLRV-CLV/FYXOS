/*
 * FYXOS — 内嵌的 init 用户程序
 * ---------------------------------------------------------------------------
 * 阶段 G 还没有文件系统，用户程序先用 .incbin 随内核一起带上。等 FYXFS 与
 * 真正的 exec 路径查找落地后，这个文件就该删掉了。
 */
    .section .rodata, "a"
    .align 16
    .globl g_init_image_start
    .globl g_init_image_end

g_init_image_start:
    .incbin "build/user/init.elf"
    .align 8
g_init_image_end:

.section .note.GNU-stack,"",@progbits
