; user/crt064.S — NanOS user startup, System V AMD64. The kernel enters at _start with rsp
; pointing at argc; argv[] follows, then NULL, then envp[]. Pass main(argc, argv, envp) in
; rdi/rsi/rdx, keep rsp 16-aligned at the call, then exit(main(...)).
; (cf. the i686 user/crt0.S which bootstraps TLS + environ for the libc.ndl path.)
[BITS 64]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __nx_init_tls_tpl]
[EXTERN __nx_set_environ]
[EXTERN __nx_set_progname]
; Per-program TLS template (defined by arch/x86_64/user-nx.ld). image is a real address; the
; sizes/align are absolute link-time constants (loaded as immediates, not relocated differences).
[EXTERN __nx_tls_image]
[EXTERN __nx_tls_filesz]
[EXTERN __nx_tls_memsz]
[EXTERN __nx_tls_align_sym]
; Global-constructor list bounds (arch/x86_64/user-nx.ld). Run before main so C++ static objects and
; __attribute__((constructor)) functions initialize — without this a program's ctors never run.
[EXTERN __nx_init_array_start]
[EXTERN __nx_init_array_end]
[EXTERN __nx_ctors_start]
[EXTERN __nx_ctors_end]
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
    ; on x86_64 — see tls.c.) The program's TLS template (.tdata/.tbss) is PER-PROGRAM, defined by
    ; user-nx.ld — libc.ndl can't reference those symbols, so we compute image/filesz/memsz/align
    ; here and pass them in. A program with no __thread data has memsz=0 -> bare-TCB fallback.
    mov rdi, __nx_tls_image             ; image = .tdata start (real, rebased address)
    mov rsi, __nx_tls_filesz            ; filesz (absolute constant from user-nx.ld)
    mov rdx, __nx_tls_memsz             ; memsz  (absolute constant; 0 => bare-TCB fallback)
    mov rcx, __nx_tls_align_sym         ; align  (absolute constant)
    call __nx_init_tls_tpl
    ; Publish the environment to libc (environ lives in libc.ndl, unreachable as a data symbol
    ; from here) so getenv()/environ work, and the program name for getprogname(). Mirrors the
    ; i686 crt0.S — without __nx_set_environ, libc's environ stays NULL and any environ deref
    ; (e.g. init's execve(..., environ)) faults.
    mov rdi, r14                    ; envp
    call __nx_set_environ
    mov rdi, [r13]                  ; argv[0]
    call __nx_set_progname
    ; Run global constructors before main (TLS/environ/progname are already live above, so ctors
    ; may use libc). Both symbol bounds and the stored pointers are rebased by mknx. rbx/r15 are
    ; callee-saved so the ctors preserve them (and argc/argv/envp in r12/r13/r14). rsp stays
    ; 16-aligned across each call. Guard against 0/-1 sentinels in case a terminator is present.
    ; .init_array runs in FORWARD order:
    mov rbx, __nx_init_array_start
    mov r15, __nx_init_array_end
.ctor_init_loop:
    cmp rbx, r15
    jae .ctor_init_done
    mov rax, [rbx]
    add rbx, 8
    test rax, rax
    jz .ctor_init_loop
    cmp rax, -1
    je .ctor_init_loop
    call rax
    jmp .ctor_init_loop
.ctor_init_done:
    ; legacy .ctors run in REVERSE order (highest address first):
    mov rbx, __nx_ctors_end
    mov r15, __nx_ctors_start
.ctor_ctors_loop:
    cmp rbx, r15
    jbe .ctor_ctors_done
    sub rbx, 8
    mov rax, [rbx]
    test rax, rax
    jz .ctor_ctors_loop
    cmp rax, -1
    je .ctor_ctors_loop
    call rax
    jmp .ctor_ctors_loop
.ctor_ctors_done:
    mov rdi, r12                    ; argc
    mov rsi, r13                    ; argv
    mov rdx, r14                    ; envp
    call main                       ; main(argc, argv, envp) -> rax
    mov rdi, rax                    ; exit code
    call exit                       ; does not return (kernel longjmps back)
.hang:
    jmp .hang
