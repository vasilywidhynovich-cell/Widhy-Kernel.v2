; Entry point kernel 64-bit -> panggil kmain() (C)
[bits 64]
extern kmain
global _start

section .text.entry
_start:
    call kmain
.hang:
    cli
    hlt
    jmp .hang
