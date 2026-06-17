; user/sigtramp64.S — the kernel enters the handler with [rsp] = this trampoline (its return
; address) and the SigContext just below. handler(signum) returns here; rsp then points at
; the SigContext, so invoke rt_sigreturn, which restores the interrupted context.
[BITS 64]
[GLOBAL __nx_sigtramp]
%define SYS_RT_SIGRETURN 15        ; must == SyscallNr.h SYS_rt_sigreturn (x86_64)
__nx_sigtramp:
    mov rax, SYS_RT_SIGRETURN
    syscall
.hang:
    jmp .hang
