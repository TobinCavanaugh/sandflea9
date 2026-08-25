bits 64

section .text
;
;section .bss
;align 16
;stack_bottom:
;    resb 16384
;stack_top:
;
section .text
global _start
extern kern_entry

_start:
    cli
    call enable_sse
    push rbp
    mov rbp, rsp
    call kern_entry
    hlt

global rdmsr
rdmsr:
    mov rcx, rdi
    rdmsr
    shl rdx, 32
    or rax, rdx
    ret

global wrmsr
wrmsr:
    mov rcx, rdi
    mov rax, rsi
    mov rdx, rsi
    shr rdx, 32
    wrmsr
    ret

global enable_sse
enable_sse:
    mov rax, cr0
    and ax, 0xFFFB
    or ax, 0x0002
    mov cr0, rax

    mov rax, cr4
    or ax, 0x0600
    mov cr4, rax

    ret

extern kern_interrupt_handler
global _isr_common_stub
_isr_common_stub:

    ; push stuff onto the stack
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8

    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp ; point to registers_t *
    call kern_interrupt_handler

    ; Pop stuff off the stack
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16 ; pop the interrupt number and the error code

    iretq

global setjmp
setjmp:
    mov [rdi], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rdx, [rsp + 8]
    mov [rdi + 48], rdx
    mov rdx, [rsp]
    mov [rdi + 56], rdx
    xor eax, eax
    ret

global longjmp
longjmp:
    mov rbx, [rdi]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov rdx, [rdi + 56]
    mov rax, rsi
    test eax, eax
    jnz .nonzero
    inc eax
.nonzero:
    jmp rdx

global task_switch_asm
task_switch_asm:
    pushfq
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp ; save current stack pointer

    ; CR3 Switch Logic
    mov rax, [rdi + 24] ; RAX = current_task->process
    mov rbx, [rsi + 24] ; RBX = next_task->process

    ; If next process is NULL or same as current, skip CR3 reload
    test rbx, rbx
    jz .skip_cr3
    cmp rax, rbx
    je .skip_cr3

    mov rcx, [rbx + 8]  ; RCX = next_task->process->cr3
    test rcx, rcx
    jz .skip_cr3
    mov rax, cr3
    cmp rax, rcx
    je .skip_cr3
    mov cr3, rcx

.skip_cr3:
    ; Load new stack pointer from next task struct AFTER target address space is active
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    popfq

    ret