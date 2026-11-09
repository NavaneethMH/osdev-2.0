BITS 32

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

%macro IRQ_NOERR 1
global irq%1
irq%1:
    push dword 0      ; Error code (not used)
    push dword %1     ; IRQ number
    jmp irq_common
%endmacro

%macro IRQ_ERR 1
global irq%1
irq%1:
    push dword %1     ; Error code
    push dword %1     ; IRQ number
    jmp irq_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; IRQs 0-31 (hardware interrupts)
IRQ_NOERR 32
IRQ_NOERR 33
IRQ_NOERR 34
IRQ_NOERR 35
IRQ_NOERR 36
IRQ_NOERR 37
IRQ_NOERR 38
IRQ_NOERR 39
IRQ_NOERR 40
IRQ_NOERR 41
IRQ_NOERR 42
IRQ_NOERR 43
IRQ_NOERR 44
IRQ_NOERR 45
IRQ_NOERR 46
IRQ_NOERR 47

extern exception_handler
extern irq_handler
isr_common:
    cli
    cld
    mov eax, [esp]
    mov edx, [esp + 4]
    push edx
    push eax
    call exception_handler
.hang:
    cli
    hlt
    jmp .hang

irq_common:
    pusha                     ; Save all general-purpose registers (8 × 4 = 32 bytes)
    mov eax, [esp + 32]      ; IRQ number: 32 bytes (pusha) offset
    push eax                  ; Push IRQ number as C argument
    call irq_handler
    add esp, 4                ; Clean up C argument
    popa                      ; Restore all general-purpose registers
    add esp, 8                ; Remove pushed error code and IRQ number
    iret                      ; Return from interrupt

global idt_load
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret
