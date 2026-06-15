; clone_helper.S — a musl-style raw clone(2) thread launcher for clonetest.
;
; The C `clone(fn==0)` idiom is unsafe: after `int 0x80` the child resumes on its OWN
; stack with a copy of the parent's registers, so any compiler assumption about a shared
; frame breaks. This naked asm helper makes the child path explicit and never lets it
; return into a C frame it does not own.
;
; int clone_thread(unsigned flags, void* child_stack_top, int* ctid, void (*fn)(void));
;
; i386 clone ABI: eax=SYS_clone(120), ebx=flags, ecx=child_stack, edx=ptid, esi=tls,
; edi=ctid. We pass ptid=tls=0 (no CLONE_PARENT_SETTID / CLONE_SETTLS). `fn` is stashed in
; ebp: the kernel (archCloneChild) copies the parent's whole trap frame into the child, so
; ebp survives into the child and points at the thread entry. The parent gets the new tid
; in eax (>0); the child gets 0, runs on child_stack_top, calls fn, then exit(0) — which,
; with two live threads, routes to the per-thread exit (CLEARTID zero + futex wake).

[BITS 32]
[GLOBAL clone_thread]

clone_thread:
    push ebx
    push esi
    push edi
    push ebp
    ; args are now 16 bytes higher on the stack (4 pushes).
    mov eax, 120            ; SYS_clone
    mov ebx, [esp + 20]     ; flags
    mov ecx, [esp + 24]     ; child_stack_top
    xor edx, edx            ; ptid = 0
    xor esi, esi            ; tls  = 0 (no CLONE_SETTLS)
    mov edi, [esp + 28]     ; ctid
    mov ebp, [esp + 32]     ; fn   (preserved into the child via the copied trap frame)
    int 0x80
    test eax, eax
    jz .child               ; eax == 0 -> we are the new thread
    ; parent: eax = child tid (or negative errno); restore and return it.
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
.child:
    ; runs on child_stack_top, ebp = fn. Call the thread function, then exit(0).
    call ebp
    mov eax, 1              ; SYS_exit
    xor ebx, ebx           ; status 0
    int 0x80
.hang:
    jmp .hang              ; exit never returns; guard against fall-through
