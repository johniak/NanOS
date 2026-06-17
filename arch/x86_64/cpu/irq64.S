; arch/x86_64/cpu/irq64.S — x86-64 hardware-IRQ entry stubs (vectors 32..47) + dispatch.
; Same manual-save / swapgs / iretq shape as isr64.S, but dispatches to irq_handler (which
; sends the PIC EOI). Scheduler-driven deferred preemption is NOT wired here (no scheduler in
; Plan 4); it returns in a later plan, mirroring i686 irq.S's schedPreempt hook.

bits 64

extern irq_handler

%macro IRQ 2
  global irq%1
  irq%1:
    push qword 0          ; err_code (IRQs carry none)
    push qword %2         ; int_no (remapped vector 32..47)
    jmp irq_common_stub
%endmacro

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

irq_common_stub:
    test byte [rsp + 24], 3
    jz .from_kernel_in
    swapgs
.from_kernel_in:

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp          ; arg0 = kernel::Registers*
    cld
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    test byte [rsp + 24], 3
    jz .from_kernel_out
    swapgs
.from_kernel_out:

    add rsp, 16
    iretq
