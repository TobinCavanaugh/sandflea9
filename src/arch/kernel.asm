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

global task_switch_asm
task_switch_asm:
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp ; save current stack pointer so [rdi] points at it
    mov rsp, [rsi] ; Load stack pointer from next task struct

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq

    ret