; NanOS signal-return trampoline. The kernel pushes a signal frame on the user stack
; ([restorer][signum][SigContext]) and enters the handler with this as its return
; address. When the handler returns, control lands here with esp pointing at signum:
; pop it (so esp -> SigContext) and invoke sigreturn, which restores the interrupted
; context from the SigContext and never returns here.
[BITS 32]
[GLOBAL __nx_sigtramp]

__nx_sigtramp:
    pop eax              ; discard signum; esp now points at the saved SigContext
    mov eax, 119         ; SYS_sigreturn
    int 0x80
.hang:
    jmp .hang            ; unreachable
