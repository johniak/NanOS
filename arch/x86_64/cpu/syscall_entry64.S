; arch/x86_64/cpu/syscall_entry64.S — SYSCALL fast-path entry (NASM elf64).
; Entry state (set by the CPU): rcx = user RIP, r11 = user RFLAGS, IF cleared by FMASK,
; CS/SS from STAR, STILL ON THE USER STACK. We swapgs to the kernel per-CPU block, switch
; to the task's kernel stack, build a kernel::Registers frame, and call into C.
;
; Per-CPU layout reached via %gs after swapgs (set in syscall_x86_64.cpp):
;   [gs:0] = kernel stack top for the current task    [gs:8] = scratch for the user rsp
[BITS 64]
[GLOBAL syscall_entry]
[EXTERN syscall_dispatch64]      ; void syscall_dispatch64(kernel::Registers*)
[EXTERN bklEnter]                ; kernel/Bkl.cpp — Big Kernel Lock (SMP serialization)
[EXTERN bklExit]

syscall_entry:
    swapgs                       ; GS.base -> kernel per-CPU (IA32_KERNEL_GS_BASE)
    mov [gs:8], rsp              ; stash the user rsp
    mov rsp, [gs:0]              ; switch to the current task's kernel stack

    ; Build a trap frame matching kernel::Registers (Plan 4). Push the SS/RSP/RFLAGS/CS/RIP
    ; tail an iretq-style frame would have, synthesized from the syscall-saved regs, so the
    ; SAME signal/exec code (archFrameToUser/archSigreturn) can manipulate it.
    push qword 0x23              ; ss  (user data, DPL3)
    push qword [gs:8]            ; rsp (the saved user rsp)
    push r11                     ; rflags (saved by syscall)
    push qword 0x2B              ; cs  (user code64, DPL3)
    push rcx                     ; rip (return address saved by syscall)
    push qword 0                 ; err_code (none for a syscall)
    push qword 0x100             ; int_no = 0x100 marks "via syscall" (distinct from int 0x80)

    push rax                     ; --- GPRs, order MUST match kernel::Registers ---
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

    call bklEnter                ; take the BKL for the whole syscall body (+ its signalDeliver)
    mov rdi, rsp                 ; arg0 = &Registers
    cld
    call syscall_dispatch64
    call bklExit                 ; release before SYSRET (a procExit/exec path won't return here,
                                 ; but its schedule()/archEnterUser already balanced the enter)

    pop r15                      ; restore GPRs (rax now holds the syscall result)
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

    add rsp, 16                  ; drop int_no + err_code
    pop rcx                      ; rip  -> rcx for sysret
    add rsp, 8                   ; skip cs
    pop r11                      ; rflags -> r11 for sysret
    pop rsp                      ; restore user rsp (skip nothing else: ss was above it)
    ; NB: after `pop rcx`(rip),`+8`(cs),`pop r11`(rflags), top is rsp then ss; `pop rsp`
    ; loads the user rsp and discards the ss slot implicitly (it was pushed below rsp).
    swapgs                       ; GS.base -> user
    o64 sysret                   ; sysretq: CS=STAR[63:48]+16|3, SS=+8|3, RIP=rcx, RFLAGS=r11
