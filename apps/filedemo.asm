BITS 32

global _start

%define SYS_WRITE 1
%define SYS_EXIT 2
%define SYS_FILE_OPEN 4
%define SYS_FILE_READ 5
%define SYS_FILE_WRITE 6
%define SYS_FILE_CLOSE 7

%define FILE_READ 1
%define FILE_WRITE 2
%define FILE_CREATE 4
%define FILE_TRUNCATE 8

section .text
_start:
    ; Create/replace the file using the kernel's persistent file API.
    mov eax, SYS_FILE_OPEN
    mov ebx, demo_filename
    mov ecx, FILE_WRITE | FILE_CREATE | FILE_TRUNCATE
    int 0x80
    test eax, eax
    jle .failed
    mov edi, eax

    mov eax, SYS_FILE_WRITE
    mov ebx, edi
    mov ecx, demo_message
    mov edx, demo_message_end - demo_message
    int 0x80
    cmp eax, demo_message_end - demo_message
    jne .failed_close

    mov eax, SYS_FILE_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .failed

    ; Reopen and read it into the writable user stack, then print the bytes.
    mov eax, SYS_FILE_OPEN
    mov ebx, demo_filename
    mov ecx, FILE_READ
    int 0x80
    test eax, eax
    jle .failed
    mov edi, eax
    sub esp, 128

    mov eax, SYS_FILE_READ
    mov ebx, edi
    mov ecx, esp
    mov edx, 128
    int 0x80
    test eax, eax
    jle .failed_read_close
    mov esi, eax

    mov eax, SYS_WRITE
    mov ebx, esp
    mov ecx, esi
    int 0x80
    cmp eax, esi
    jne .failed_read_close

    mov eax, SYS_FILE_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .failed
    add esp, 128
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80

.failed_read_close:
    mov eax, SYS_FILE_CLOSE
    mov ebx, edi
    int 0x80
    add esp, 128
    jmp .failed

.failed_close:
    mov eax, SYS_FILE_CLOSE
    mov ebx, edi
    int 0x80
.failed:
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
.halt:
    cli
    hlt
    jmp .halt

demo_filename:
    db "api-demo.txt", 0
demo_message:
    db "File API write/read succeeded.", 10
demo_message_end:

section .note.GNU-stack noalloc noexec nowrite progbits
