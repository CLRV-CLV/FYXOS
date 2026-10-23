/*
 * stage1 链接脚本：固定 512 字节的 MBR。
 * 0x1B0 起 14 字节为引导描述符，0x1BE 起 64 字节为分区表，
 * 0x1FE 为 0xAA55 引导签名（均由 tools/mkimage.py 填充）。
 */
OUTPUT_FORMAT("binary")
OUTPUT_ARCH(i386)

SECTIONS
{
    . = 0x7C00;

    .text   : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) }

    . = 0x7DB0;
    .boot_descriptor (NOLOAD) : { . = . + 14; }

    . = 0x7DBE;
    .partition_table (NOLOAD) : { . = . + 64; }

    . = 0x7DFE;
    .boot_signature : { SHORT(0xAA55) }

    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) *(.debug*) }
}
