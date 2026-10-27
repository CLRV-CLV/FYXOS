/*
 * FYXOS — 用户程序链接脚本
 *
 * 固定链接到 0x400000（非 PIE）。各段之间补齐到页边界，这样内核的 ELF
 * 装载器给每段单独设 W^X 权限时不会把两个段塞进同一页 —— 否则先后的映射会
 * 互相覆盖，"只读数据"可能被标成可执行。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text   : { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) }
    . = ALIGN(0x1000);
    .bss    : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(0x1000);

    /DISCARD/ : { *(.note.GNU-stack) *(.eh_frame) *(.comment) }
}
