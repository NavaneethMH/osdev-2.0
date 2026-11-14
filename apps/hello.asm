BITS 32

global _start

section .text
_start:
    mov eax, 1                  ; SYS_WRITE
    mov ebx, hello_message
    mov ecx, hello_message_end - hello_message
    int 0x80

    mov eax, 2                  ; SYS_EXIT
    mov ebx, 37                 ; Deliberate demo status for the shell.
    int 0x80

.unexpected_return:
    cli
    hlt
    jmp .unexpected_return

hello_message:
    db "Hello from the embedded TinyOS ELF app.", 10
hello_message_end:

section .note.GNU-stack noalloc noexec nowrite progbits
