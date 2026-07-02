; NanOS user program startup. The kernel enters at _start with esp pointing at an
; argv+envp image (argc at [esp], argv at esp+4, then a NULL, then envp; SysV i386).
; Publish the environment to libc (for getenv), then call main(argc, argv, envp), then
; exit. environ lives in libc.ndl, unreachable as a data symbol from here, so we hand it
; over through the exported __nx_set_environ function.
; (picolibc's __libc_init_array is added when libnanos -> picolibc.)
[BITS 32]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __nx_init_tls]
[EXTERN __nx_set_environ]
[EXTERN __nx_set_progname]

_start:
    mov ebp, 0
    ; Install the main thread's TLS (TCB + %gs:0) FIRST, before any libc work — errno is
    ; (*__errno_location()) which reads the TCB at %gs:0, so the thread pointer must be
    ; live before the first libc call. (libc.ndl exports __nx_init_tls; see tls.c.) i386 programs
    ; carry no __thread data, so the legacy 0-arg bare-TCB entry is exactly right. (Program TLS is
    ; an x86_64 feature: x86_64 crt064.S calls __nx_init_tls_tpl with the user-nx.ld template.)
    call __nx_init_tls
    mov eax, [esp]              ; argc
    lea edx, [esp + 4]         ; argv
    lea ecx, [esp + 8 + eax*4] ; envp  (= esp + 4 + (argc+1)*4)
    push ecx                    ; main arg3: envp
    push edx                    ; main arg2: argv
    push eax                    ; main arg1: argc
    push ecx                    ; __nx_set_environ(envp)
    call __nx_set_environ       ; publish the environment for getenv()
    add esp, 4                  ; drop its arg; argc/argv/envp remain staged for main
    mov edx, [esp + 4]          ; reload argv (edx is caller-saved across __nx_set_environ)
    mov eax, [edx]              ; argv[0]
    push eax                    ; __nx_set_progname(argv[0]) so getprogname() works
    call __nx_set_progname      ; (gnulib's error()/coreutils use getprogname for the prefix)
    add esp, 4
    call main                   ; main(argc, argv, envp)
    add esp, 12
    push eax                    ; main's return value
    call exit                   ; does not return (loader longjmps back to kernel)
.hang:
    jmp .hang
