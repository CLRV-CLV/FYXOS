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

    /* 由配套开源 GCC C11 SDK 生成的独立示例，不是 fyxcc 的产物。 */
    .align 16
    .globl g_gcc_demo_image_start
    .globl g_gcc_demo_image_end
g_gcc_demo_image_start:
    .incbin "build/sdk/c11-demo.elf"
    .align 8
g_gcc_demo_image_end:

    /* 标准 ar + gzip tar 的 amd64 DEB；启动时作为普通包文件安装到 FYXFS。 */
    .align 16
    .globl g_deb_demo_image_start
    .globl g_deb_demo_image_end
g_deb_demo_image_start:
    .incbin "build/compat/fyx-deb-demo.deb"
g_deb_demo_image_end:
    .align 8

    /* 小型未压缩包持续覆盖 control.tar/data.tar 原始路径。 */
    .align 16
    .globl g_deb_plain_image_start
    .globl g_deb_plain_image_end
g_deb_plain_image_start:
    .incbin "build/compat/fyx-deb-plain-demo.deb"
g_deb_plain_image_end:
    .align 8

    /* data.tar.gz CRC32 损坏；必须在写任何文件之前拒绝。 */
    .align 16
    .globl g_deb_bad_image_start
    .globl g_deb_bad_image_end
g_deb_bad_image_start:
    .incbin "build/compat/fyx-deb-bad-gzip.deb"
g_deb_bad_image_end:
    .align 8

    /* 真正的 PE32+：经 kernel32.dll 的三项导入进入 Win32 服务分表。 */
    .align 16
    .globl g_win64_demo_image_start
    .globl g_win64_demo_image_end
g_win64_demo_image_start:
    .incbin "build/compat/win64-demo.exe"
    .align 8
g_win64_demo_image_end:

    .align 16
    .globl g_win64_bad_image_start
    .globl g_win64_bad_image_end
g_win64_bad_image_start:
    .incbin "build/compat/win64-bad-import.exe"
g_win64_bad_image_end:

.section .note.GNU-stack,"",@progbits
