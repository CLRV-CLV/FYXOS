/*
 * FYXOS — ELF64 最小定义（引导器与内核符号解析共用）
 */
#ifndef FYXOS_ELF_H
#define FYXOS_ELF_H

#include <fyxos/types.h>

#define ELF_MAGIC   0x464C457Ful   /* "\x7FELF" */
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_X86_64   62
#define ET_EXEC     2
#define ET_DYN      3

#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4

#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

#define SHT_SYMTAB  2
#define SHT_STRTAB  3

struct elf64_ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} PACKED;

struct elf64_phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} PACKED;

struct elf64_shdr {
    u32 sh_name;
    u32 sh_type;
    u64 sh_flags;
    u64 sh_addr;
    u64 sh_offset;
    u64 sh_size;
    u32 sh_link;
    u32 sh_info;
    u64 sh_addralign;
    u64 sh_entsize;
} PACKED;

struct elf64_sym {
    u32 st_name;
    u8  st_info;
    u8  st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} PACKED;

#endif /* FYXOS_ELF_H */
