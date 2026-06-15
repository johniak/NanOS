%macro ISR_NOERRCODE 1  ; define a macro, taking one parameter
  [GLOBAL isr%1]        ; %1 accesses the first parameter.
  isr%1:
    cli
    push byte 0
    push byte %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
  [GLOBAL isr%1]
  isr%1:
    cli
    push byte %1
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE 8
ISR_NOERRCODE 9
ISR_ERRCODE 10
ISR_ERRCODE 11
ISR_ERRCODE 12
ISR_ERRCODE 13
ISR_ERRCODE 14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_NOERRCODE 17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; Syscall entry (vector 0x80 = 128). Use `push dword` — `push byte 128` would
; sign-extend to 0xFFFFFF80 and corrupt the interrupt number.
[GLOBAL isr128]
isr128:
    cli
    push dword 0
    push dword 128
    jmp isr_common_stub



isr_common_stub:
   pusha                    ; Pushes edi,esi,ebp,esp,ebx,edx,ecx,eax

   mov ax, ds               ; Lower 16-bits of eax = ds.
   push eax                 ; save the data segment descriptor

   mov ax, 0x10  ; load the kernel data segment descriptor
   mov ds, ax
   mov es, ax
   mov fs, ax
   mov gs, ax
   [extern isr_handler]
   call isr_handler

; A freshly forked child resumes here: archForkChild planted a copied trap frame so
; that, after the scheduler's context switch `ret`s into this label, the tail below
; restores segments + registers and `iret`s the child into ring 3 with eax = 0.
[GLOBAL ret_from_fork]
ret_from_fork:
   pop eax        ; reload the original data segment descriptor
   mov ds, ax
   mov es, ax
   mov fs, ax
   mov gs, ax

   ; Per-thread TLS: returning to ring 3, point %gs at the TLS slot (selector 0x33,
   ; GDT entry 6 whose base the scheduler/set_thread_area keep aimed at the current
   ; thread's TLS block) instead of mirroring the user data selector. Without this the
   ; line above would reset %gs to the saved ds (0x23) on every syscall return, wiping
   ; the thread pointer (see Gdt::setTlsBase). Saved CS sits at [esp+44] now (esp+0..31
   ; = pusha, +32 = int_no, +36 = err_code, +40 = eip, +44 = cs). Ring 0 returns are
   ; left untouched (the kernel never uses %gs for addressing).
   test dword [esp + 44], 3
   jz .ret_kernel_gs
   mov ax, 0x33
   mov gs, ax
.ret_kernel_gs:

   popa                     ; Pops edi,esi,ebp...
   add esp, 8     ; Cleans up the pushed error code and pushed ISR number
   sti
   iret         