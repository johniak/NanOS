; NanOS user program startup. Sets up the user stack, calls main, then exit.
[BITS 32]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]

_start:
    mov esp, 0x500000        ; user stack top (region 0x400000-0x4FFFFF)
    call main
    push eax                 ; main's return value
    call exit                ; does not return (loader longjmps back to kernel)
.hang:
    jmp .hang
