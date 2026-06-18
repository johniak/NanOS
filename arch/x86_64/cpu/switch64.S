; switch64.S — x86_64 context switch + task trampoline + fork return path.
; NASM (Intel), elf64. Symbols are bare (no leading underscore), matching the kernel ABI.
; Translated from arch/x86/cpu/switch.S; the only register-set change is the SysV AMD64
; callee-saved set (rbx, rbp, r12, r13, r14, r15) — caller-saved regs need not survive a
; call-convention switch — plus CR3 (the switch may change the address space).
[BITS 64]
[GLOBAL archContextSwitch]
[GLOBAL taskTrampoline]
[GLOBAL ret_from_fork]
[EXTERN schedulerRunCurrentBody]

; void archContextSwitch(uintptr_t* saveOldKesp /*rdi*/, uintptr_t newKesp /*rsi*/)
;   Save the callee-saved GPRs + CR3 + rsp of the current task into *saveOldKesp, then load
;   the next task's context. Stack after the 7 pushes (low -> high address, rsp at the low end):
;     [rsp]      cr3   (popped first)
;     [rsp+8]    r15
;     [rsp+16]   r14
;     [rsp+24]   r13
;     [rsp+32]   r12
;     [rsp+40]   rbp
;     [rsp+48]   rbx
;     [rsp+56]   return address
;   archTaskBootstrap and archForkChild fabricate stacks that MUST match this pop order.
archContextSwitch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov rax, cr3
    push rax
    mov [rdi], rsp           ; *saveOldKesp = current rsp
    mov rsp, rsi             ; rsp = newKesp (switch stacks)
    pop rax                  ; rax = the next task's saved cr3
    mov rcx, cr3             ; rcx = the current (outgoing) cr3
    cmp rax, rcx
    je .samecr3              ; same address space (e.g. kernel thread <-> kernel thread):
    mov cr3, rax             ;   only reload — and flush the TLB — when it actually changes
.samecr3:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                      ; resume the task (its preempted frame / trampoline / fork return)

; First entry of a freshly bootstrapped task (archTaskBootstrap planted this as the "return
; address"). Enable interrupts so it can be preempted, then run its body. The fabricated stack
; leaves rsp 16-byte aligned here (the scheduler hands a 16-aligned kstackTop), so the call
; below satisfies the SysV alignment contract.
taskTrampoline:
    sti
    call schedulerRunCurrentBody
.hang:
    hlt
    jmp .hang

; A freshly forked/cloned child resumes here: archForkChild planted a copied kernel::Registers
; trap frame so that, after the scheduler's context switch `ret`s into this label, the tail
; below restores the 15 GPRs and `iretq`s the child into ring 3 with rax = 0. Mirrors the tail
; of isr64.S's isr_common_stub (manual GPR restore + ring-3 swapgs + drop int_no/err + iretq).
; On entry rsp points at the lowest Registers field (r15).
ret_from_fork:
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
    ; rsp now points at int_no; saved CS is at rsp+24. swapgs back to the user GS base only when
    ; returning to ring 3 (mirror of the isr stub's entry test).
    test byte [rsp + 24], 3
    jz .rff_kernel
    swapgs
.rff_kernel:
    add rsp, 16              ; discard int_no + err_code
    iretq                    ; restores rip/cs/rflags/rsp/ss (IF comes from the copied rflags)
