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
[EXTERN schedForkFinish]         ; kernel/Scheduler.cpp — finish_task_switch for the fork child

; void archContextSwitch(uintptr_t* saveOldKesp /*rdi*/, uintptr_t newKesp /*rsi*/,
;                        void* saveOldFx /*rdx*/, void* loadNewFx /*rcx*/)
;   Save the callee-saved GPRs + CR3 + rsp of the current task into *saveOldKesp, then load
;   the next task's context.
;   rdx/rcx = the outgoing/incoming tasks' 512-byte 16-aligned FXSAVE areas (null = skip).
;   Ring 3 is built SSE-ON while the kernel is -mno-sse, so the live XMM0-15/MXCSR/x87 state
;   here is exactly the interrupted user task's — it MUST travel with the task: a deferred
;   preemption on ret-to-ring3 lands mid-computation (not at a call boundary), so without this
;   the resumed task continued with ANOTHER task's registers (silent data corruption; on the
;   Dell: "impossible" SIGSEGVs inside a fuzz-proven PNG decoder under boot context-switch
;   pressure). fxsave runs before the GPR pushes (pushes don't touch XMM); fxrstor runs right
;   after the stack switch while rcx is still intact (the CR3 compare below clobbers rcx) —
;   both areas are identity-mapped kernel memory visible under either CR3, so ordering vs the
;   address-space reload is free.
;   Stack after the 7 pushes (low -> high address, rsp at the low end):
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
    test rdx, rdx
    jz .nofxsave
    fxsave [rdx]             ; park the outgoing task's FPU/SSE state in its own area
.nofxsave:
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
    test rcx, rcx
    jz .nofxrstor
    fxrstor [rcx]            ; load the incoming task's FPU/SSE state (before rcx is clobbered)
.nofxrstor:
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
    ; finish_task_switch: release the task this CPU switched away from (the child resumes
    ; straight into ring 3 below, never re-entering schedule(), so it runs the handoff here).
    ; rsp is 16-aligned at this point (the fabricated Registers frame is, so the call is ABI-ok);
    ; the GPRs restored below come off the stack, so schedForkFinish's clobbers don't matter.
    call schedForkFinish
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
