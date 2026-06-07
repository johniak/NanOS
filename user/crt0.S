; NanOS user program startup. The kernel enters at _start with esp pointing at an
; argv image (argc at [esp], argv at esp+4, SysV i386). Call main(argc, argv),
; then exit. (picolibc's __libc_init_array is added when libnanos -> picolibc.)
[BITS 32]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]

_start:
    mov ebp, 0
    mov eax, [esp]          ; argc
    lea edx, [esp + 4]      ; argv
    push edx                ; argv
    push eax                ; argc
    call main               ; main(argc, argv)
    add esp, 8
    push eax                ; main's return value
    call exit               ; does not return (loader longjmps back to kernel)
.hang:
    jmp .hang
