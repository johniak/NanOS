; arch/x86_64/cpu/nxjmp64.S — minimal setjmp/longjmp so a user program's exit() can
; return control to the kernel after it switched to its own stack (x86_64 SysV).
; struct NxJmp { u64 rsp, rbp, rbx, r12, r13, r14, r15, rip }  (offsets 0,8,16,...,56)
[BITS 64]
[GLOBAL nx_setjmp]
[GLOBAL nx_longjmp]

; long nx_setjmp(NxJmp* buf)  — rdi=buf; returns 0 here, returns `val` when longjmp'd to.
nx_setjmp:
    mov [rdi+0],  rsp        ; rsp points at the return address right now
    mov [rdi+8],  rbp
    mov [rdi+16], rbx
    mov [rdi+24], r12
    mov [rdi+32], r13
    mov [rdi+40], r14
    mov [rdi+48], r15
    mov rax, [rsp]           ; return address
    mov [rdi+56], rax
    xor eax, eax
    ret

; void nx_longjmp(NxJmp* buf, long val) — rdi=buf, rsi=val; resume at the setjmp site.
nx_longjmp:
    mov rax, rsi             ; return value
    mov rbp, [rdi+8]
    mov rbx, [rdi+16]
    mov r12, [rdi+24]
    mov r13, [rdi+32]
    mov r14, [rdi+40]
    mov r15, [rdi+48]
    mov rsp, [rdi+0]         ; restore stack ([rsp] = saved return address)
    mov rdx, [rdi+56]
    mov [rsp], rdx           ; ensure we ret to the setjmp site
    ret
