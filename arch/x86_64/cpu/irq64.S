; arch/x86_64/cpu/irq64.S — x86-64 hardware-IRQ entry stubs (vectors 32..47) + dispatch.
; Same manual-save / swapgs / iretq shape as isr64.S, but dispatches to irq_handler (which
; sends the PIC EOI). Scheduler-driven deferred preemption IS wired here now (mirroring i686
; irq.S's schedPreempt hook): on the way back to ring 3, irq_common_stub calls schedPreempt so a
; quantum-expired user task is involuntarily preempted. Without this, a CPU-/syscall-bound user
; task (e.g. a busy-looping daemon) never yields and starves every other task — including the net
; softirq thread and the console — under the otherwise cooperative scheduler.

bits 64

extern irq_handler
extern schedPreempt

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

    ; Deferred preemption: switch tasks ONLY on the way back to ring 3, where the kernel stack
    ; holds a full, clean trap frame. With the 15 GP regs still pushed, the saved CS sits at
    ; [rsp + 15*8 + int_no(8) + err_code(8) + RIP(8)] = [rsp+144]. Skip when returning to ring 0
    ; (kernel is never preempted mid-instruction). schedPreempt may context-switch; on resume we
    ; fall through to the pops below, so every GP reg (incl. schedPreempt's caller-saved clobbers)
    ; is restored from the stack before iretq. GS is still the kernel GS here (swapgs is below).
    test byte [rsp + 144], 3
    jz .no_preempt
    call schedPreempt
.no_preempt:

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
