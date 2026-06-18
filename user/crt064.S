; user/crt064.S — NanOS user startup, System V AMD64. The kernel enters at _start with rsp
; pointing at argc; argv[] follows, then NULL, then envp[]. Pass main(argc, argv, envp) in
; rdi/rsi/rdx, keep rsp 16-aligned at the call, then exit(main(...)).
; (cf. the i686 user/crt0.S which bootstraps TLS + environ for the libc.ndl path.)
[BITS 64]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __nx_init_tls]
_start:
    xor rbp, rbp
    ; Install the main thread's TLS (TCB + %fs.base) FIRST, before any libc work — errno is
    ; (*__errno_location()) which reads the TCB at %fs:0, so the thread pointer must be live
    ; before the first libc call. (libc.ndl exports __nx_init_tls; it arch_prctl(ARCH_SET_FS)
    ; on x86_64 — see tls.c.) Preserve rsp across the call; it is callee-clobbered otherwise.
    call __nx_init_tls
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    lea rdx, [rsp + 8 + rdi*8 + 8]  ; envp = argv + (argc+1)*8
    and rsp, -16                ; 16-align before the call (ABI)
    call main                   ; main(argc, argv, envp) -> rax
    mov rdi, rax                ; exit code
    call exit                   ; does not return (kernel longjmps back)
.hang:
    jmp .hang
