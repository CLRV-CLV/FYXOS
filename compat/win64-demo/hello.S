/*
 * Win64 PE32+ compatibility payload, GNU assembler (no MinGW required).
 * Uses Microsoft x64 calls through a real kernel32.dll import directory.
 */
.intel_syntax noprefix
.section .text,"ax"
.p2align 4
section_start:
.globl _start
_start:
    /* A Windows image can read its own headers at ImageBase. Verify MZ/PE. */
    mov r10d, 0x400000
    cmp WORD PTR [r10], 0x5a4d
    jne win64_error
    mov ecx, DWORD PTR [r10 + 0x3c]
    cmp DWORD PTR [r10 + rcx], 0x00004550
    jne win64_error

    /* RSP starts 16-byte aligned. 48 = 32-byte shadow space + two arg slots. */
    and rsp, -16
    sub rsp, 48

    mov ecx, -11                    /* STD_OUTPUT_HANDLE */
    call QWORD PTR iat_GetStdHandle[rip]
    cmp rax, -1
    je win64_error
    mov rbx, rax

    mov rcx, rbx
    lea rdx, message[rip]
    mov r8d, message_end - message
    lea r9, [rsp + 40]              /* LPDWORD lpNumberOfBytesWritten */
    mov QWORD PTR [rsp + 32], 0      /* lpOverlapped = NULL */
    call QWORD PTR iat_WriteFile[rip]
    test eax, eax
    jz win64_error
    cmp DWORD PTR [rsp + 40], message_end - message
    jne win64_error

    mov ecx, 24
    call QWORD PTR iat_ExitProcess[rip]
win64_error:
    mov ecx, 90
    call QWORD PTR iat_ExitProcess[rip]
    jmp win64_error

.p2align 4
message:
    .ascii "WIN64_PE_OK\r\nWIN32_IMPORT_OK\r\n"
message_end:

/* Fixed section offset: tools/mkpe64.py advertises import RVA 0x1400. */
    . = section_start + 0x400
import_directory:
    .long import_lookup - section_start + 0x1000 /* OriginalFirstThunk */
    .long 0                                      /* TimeDateStamp */
    .long 0                                      /* ForwarderChain */
    .long kernel32_name - section_start + 0x1000
    .long import_address - section_start + 0x1000/* FirstThunk */
    .long 0, 0, 0, 0, 0                         /* NULL descriptor */

.p2align 3
import_lookup:
    .quad hint_GetStdHandle - section_start + 0x1000
    .quad hint_WriteFile - section_start + 0x1000
    .quad hint_ExitProcess - section_start + 0x1000
    .quad 0

.p2align 3
import_address:
iat_GetStdHandle:
    .quad hint_GetStdHandle - section_start + 0x1000
iat_WriteFile:
    .quad hint_WriteFile - section_start + 0x1000
iat_ExitProcess:
    .quad hint_ExitProcess - section_start + 0x1000
    .quad 0

kernel32_name:
    .asciz "kernel32.dll"
.p2align 1
hint_GetStdHandle:
    .word 0
    .asciz "GetStdHandle"
.p2align 1
hint_WriteFile:
    .word 0
    .asciz "WriteFile"
.p2align 1
hint_ExitProcess:
    .word 0
    .asciz "ExitProcess"
.p2align 4
section_end:

.section .note.GNU-stack,"",@progbits
