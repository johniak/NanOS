; user/crt064.S — NanOS user startup, System V AMD64. The kernel enters at _start with rsp
; pointing at argc; argv[] follows, then NULL, then envp[]. Pass main(argc, argv, envp) in
; rdi/rsi/rdx, keep rsp 16-aligned at the call, then exit(main(...)).
; (cf. the i686 user/crt0.S which bootstraps TLS + environ for the libc.ndl path.)
[BITS 64]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __nx_init_tls]
[EXTERN __nx_set_environ]
[EXTERN __nx_set_progname]
_start:
    xor rbp, rbp
    ; Capture argc/argv/envp from the entry stack into callee-saved regs BEFORE touching rsp
    ; or making any call (rdi/rsi/rdx are caller-clobbered). envp = argv + (argc+1)*8.
    mov r12, [rsp]                  ; argc
    lea r13, [rsp + 8]              ; argv
    lea r14, [rsp + 8 + r12*8 + 8]  ; envp
    and rsp, -16                    ; 16-align rsp for every subsequent ABI call
    ; Install the main thread's TLS (TCB + %fs.base) FIRST, before any libc work — errno is
    ; (*__errno_location()) which reads the TCB at %fs:0, so the thread pointer must be live
    ; before the first libc call. (libc.ndl exports __nx_init_tls; it arch_prctl(ARCH_SET_FS)
    ; on x86_64 — see tls.c.)
    call __nx_init_tls
    ; Publish the environment to libc (environ lives in libc.ndl, unreachable as a data symbol
    ; from here) so getenv()/environ work, and the program name for getprogname(). Mirrors the
    ; i686 crt0.S — without __nx_set_environ, libc's environ stays NULL and any environ deref
    ; (e.g. init's execve(..., environ)) faults.
    mov rdi, r14                    ; envp
    call __nx_set_environ
    mov rdi, [r13]                  ; argv[0]
    call __nx_set_progname
    mov rdi, r12                    ; argc
    mov rsi, r13                    ; argv
    mov rdx, r14                    ; envp
    call main                       ; main(argc, argv, envp) -> rax
    mov rdi, rax                    ; exit code
    call exit                       ; does not return (kernel longjmps back)
.hang:
    jmp .hang
