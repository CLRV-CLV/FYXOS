/*
 * FYXOS 内核链接脚本
 * ---------------------------------------------------------------------------
 * 虚拟地址（VMA）：0xFFFFFFFF80000000 —— 经典 x86-64 高半核
 * 物理地址（LMA）：0x200000          —— 2 MiB 对齐，便于 stage2 用 2 MiB 大页映射
 *
 * stage2 依据程序头的 p_paddr 搬移段，因此每个可加载段都必须显式指定 AT()。
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_kernel_entry)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x0000000000200000;

SECTIONS
{
    . = KERNEL_VMA;

    .text BLOCK(4K) : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA) {
        _text_start = .;
        *(.text.entry)
        *(.text*)
        _text_end = .;
    }

    .rodata BLOCK(4K) : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA) {
        _rodata_start = .;
        *(.rodata*)
        _rodata_end = .;
    }

    .data BLOCK(4K) : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA) {
        _data_start = .;
        *(.data*)
        _data_end = .;
    }

    .bss BLOCK(4K) : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA) {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    /* 引导期临时栈：stage2 跳入内核后、内核建立自己的内核栈之前使用 */
    . = ALIGN(16);
    .boot_stack BLOCK(4K) (NOLOAD) : AT(ADDR(.boot_stack) - KERNEL_VMA + KERNEL_LMA) {
        _boot_stack_bottom = .;
        . = . + 64K;
        _boot_stack_top = .;
    }

    _kernel_end = .;
    _kernel_phys_end = _kernel_end - KERNEL_VMA + KERNEL_LMA;

    /DISCARD/ : {
        *(.eh_frame*) *(.comment*) *(.note*) *(.debug*) *(.got*) *(.plt*)
    }
}
