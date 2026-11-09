BITS 32

section .multiboot
align 4
    dd 0x1BADB002
    dd 0x00000003
    dd -(0x1BADB002 + 0x00000003)

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    ; Multiboot gives the boot magic in EAX and the info pointer in EBX.
    ; Push C arguments right-to-left: kernel_main(magic, info_address).
    push ebx
    push eax
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
