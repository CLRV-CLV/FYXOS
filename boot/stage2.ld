/*
 * stage2 链接脚本：0x8000 处的 flat binary，最大 32752 字节
 * （末尾 16 字节留给 mkimage.py 写入 stage2 描述符）。
 *
 * 段顺序至关重要：.text16 必须在最前面，因为 stage1 直接 jmp 到 0x8000。
 */
OUTPUT_FORMAT("binary")
OUTPUT_ARCH(i386)

SECTIONS
{
    . = 0x8000;

    .text : {
        *(.text16)
        . = ALIGN(16);
        *(.text32)
        . = ALIGN(16);
        *(.text64)
        . = ALIGN(16);
        *(.text*)
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    __stage2_end = .;

    /DISCARD/ : {
        *(.eh_frame*) *(.comment*) *(.note*) *(.debug*) *(.got*) *(.plt*)
    }
}

ASSERT(__stage2_end <= 0x8000 + 32752, "stage2 exceeds 32752 bytes")
