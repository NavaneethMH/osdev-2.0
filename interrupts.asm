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

%macro SYSCALL_NOERR 1
global syscall%1
syscall%1:
    push dword 0
    push dword %1
    jmp syscall_common
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

; User-callable system-call gate (int 0x80).
SYSCALL_NOERR 128

extern exception_handler
extern irq_handler
extern gdt_set_kernel_stack
extern syscall_handler
isr_common:
    cli
    cld
    pusha
    lea eax, [esp + 32]
    push eax
    push dword [eax + 4]
    push dword [eax]
    call exception_handler
    add esp, 12
    popa
    add esp, 8
    iret

irq_common:
    pusha                     ; Save all general-purpose registers (8 × 4 = 32 bytes)
    mov eax, esp              ; Preserve the complete frame for the C scheduler
    mov edx, [esp + 32]       ; IRQ vector: 32 bytes (pusha) offset
    push eax                  ; Second argument: saved register-frame pointer
    push edx                  ; First argument: IRQ vector
    call irq_handler
    add esp, 8                ; Clean up C arguments
    mov esp, eax              ; Restore the selected task's interrupt frame
    popa                      ; Restore all general-purpose registers
    add esp, 8                ; Remove pushed error code and IRQ number
    iret                      ; Return from interrupt

syscall_common:
    cli
    cld
    pusha
    push esp                  ; syscall_handler(struct syscall_cpu_frame *)
    call syscall_handler
    add esp, 4
    popa
    add esp, 8                ; Remove vector and synthetic error code
    iret

global gdt_load
gdt_load:
    mov eax, [esp + 4]
    lgdt [eax]
    jmp 0x08:.reload_cs
.reload_cs:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ax, 0x28
    ltr ax
    ret

; Enter ring 3 with paging left enabled. Callers must provide mapped user
; code and stack pages; the kernel identity map stays supervisor-only.
global enter_user_mode
enter_user_mode:
    cli
    push ebx
    push esi
    mov ebx, [esp + 12]       ; entry argument
    mov esi, [esp + 16]       ; user stack argument
    mov eax, esp              ; TSS ring-0 stack top (saved-register frame)
    push eax
    call gdt_set_kernel_stack
    add esp, 4

    mov ax, 0x23              ; user data selector (RPL 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push dword 0x23           ; user SS
    push esi                  ; user ESP
    push dword 0x2            ; safe EFLAGS: reserved bit set, IRQs disabled
    push dword 0x1B           ; user CS (RPL 3)
    push ebx                  ; user EIP

    iretd

global ring3_demo_return_trampoline
ring3_demo_return_trampoline:
    add esp, 8                ; discard user ESP/SS left by the privilege IRET
    mov ax, 0x10              ; restore kernel data selectors
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop esi
    pop ebx
    ret

; This contiguous blob is copied into a dedicated read-only user page.
global user_blob_start
user_blob_start:
; Controlled ring-3 privilege transition proof.
global ring3_demo_user
ring3_demo_user:
    int3
.user_hang:
    cli
    hlt
    jmp .user_hang

; Controlled user-mode ABI exercise. Derive the message address from the
; current instruction pointer so this code remains relocatable when copied.
global ring3_syscall_demo_user
ring3_syscall_demo_user:
    call .get_base
.get_base:
    pop esi
    mov eax, 1                ; SYS_WRITE
    lea ebx, [esi + ring3_syscall_message - .get_base]
    mov ecx, ring3_syscall_message_end - ring3_syscall_message
    int 0x80
    mov eax, 3                ; SYS_YIELD
    int 0x80
    mov eax, 2                ; SYS_EXIT
    int 0x80
.syscall_demo_hang:
    cli
    hlt
    jmp .syscall_demo_hang

global ring3_syscall_message
ring3_syscall_message:
    db "Ring 3 says hello through write syscall.", 10
global ring3_syscall_message_end
ring3_syscall_message_end:

; Deliberately attempt a ring-3 write to supervisor-only VGA memory. The
; kernel contains this expected page fault and returns through the TSS stack.
global ring3_protection_demo_user
ring3_protection_demo_user:
    mov dword [0x000B8000], 0xDEADBEEF
.protection_hang:
    cli
    hlt
    jmp .protection_hang
global user_blob_end
user_blob_end:

global idt_load
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; Save the callee-saved registers and stack pointer for the current task,
; then restore another task's stack. The restored stack ends in a return
; address, so this works both for resumed tasks and freshly prepared stacks.
global task_switch
task_switch:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, [esp + 20]       ; old_context argument
    mov [eax + 12], esp       ; struct task_context.esp
    mov ecx, [esp + 24]       ; new_context argument
    mov esp, [ecx + 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
