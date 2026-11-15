BITS 32

section .multiboot
align 4
    dd 0x1BADB002
    ; Request page-aligned modules, memory information, and a graphics mode.
    dd 0x00000007
    dd -(0x1BADB002 + 0x00000007)
    dd 0                    ; linear graphics mode
    dd 1024                 ; requested width
    dd 768                  ; requested height
    dd 32                   ; requested color depth

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

