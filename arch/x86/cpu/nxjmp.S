; Minimal setjmp/longjmp so a user program's exit() can return control to the
; kernel even after it switched to its own stack.
; struct NxJmp { unsigned esp, ebp, ebx, esi, edi, eip; }  (offsets 0,4,8,12,16,20)
[BITS 32]
[GLOBAL nx_setjmp]
[GLOBAL nx_longjmp]

; int nx_setjmp(NxJmp* buf) — returns 0 here, returns `val` when longjmp'd to.
nx_setjmp:
    mov ecx, [esp+4]      ; buf
    mov [ecx+0], esp      ; esp points at the return address right now
    mov [ecx+4], ebp
    mov [ecx+8], ebx
    mov [ecx+12], esi
    mov [ecx+16], edi
    mov edx, [esp]        ; return address
    mov [ecx+20], edx
    xor eax, eax
    ret

; void nx_longjmp(NxJmp* buf, int val) — resume at the setjmp site with eax=val.
nx_longjmp:
    mov ecx, [esp+4]      ; buf
    mov eax, [esp+8]      ; val
    mov ebp, [ecx+4]
    mov ebx, [ecx+8]
    mov esi, [ecx+12]
    mov edi, [ecx+16]
    mov esp, [ecx+0]      ; restore stack ([esp] = saved return address)
    mov edx, [ecx+20]
    mov [esp], edx        ; ensure we ret to the setjmp site
    ret
