; arch/x86_64/cpu/isr64.S — x86-64 CPU-exception entry stubs (vectors 0..31) + dispatch.
; No pusha/popa in long mode: save the 15 GPRs by hand into a kernel::Registers frame, pass
; &frame in rdi to isr_handler, restore, iretq. swapgs is toggled only across ring3<->ring0
; (tested via the saved CS). The int 0x80 / syscall gate is intentionally NOT here (Plan 6).

bits 64

extern isr_handler

; No-error-code vector: push a dummy 0 so every frame has the same shape.
%macro ISR_NOERRCODE 1
  global isr%1
  isr%1:
    push qword 0          ; err_code (dummy)
    push qword %1         ; int_no
    jmp isr_common_stub
%endmacro

; Error-code vector: the CPU already pushed the error code; just push the vector number.
%macro ISR_ERRCODE 1
  global isr%1
  isr%1:
    push qword %1         ; int_no
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
ISR_ERRCODE   8           ; #DF
ISR_NOERRCODE 9
ISR_ERRCODE   10          ; #TS
ISR_ERRCODE   11          ; #NP
ISR_ERRCODE   12          ; #SS
ISR_ERRCODE   13          ; #GP
ISR_ERRCODE   14          ; #PF
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

isr_common_stub:
    ; At this point the stack (from rsp) is: [int_no][err][rip][cs][rflags][rsp][ss].
    ; Saved CS is at rsp+24 -> swapgs only if we came from ring 3 (CPL bits set).
    test byte [rsp + 24], 3
    jz .from_kernel_in
    swapgs
.from_kernel_in:

    ; Manually save the 15 GPRs (r15 ends at the lowest address = first Registers field).
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp          ; arg0 = kernel::Registers* (rsp is 16-byte aligned; see plan note)
    cld                   ; System V: DF must be clear on entry to C
    call isr_handler

    ; Restore the 15 GPRs in reverse push order.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; rsp now points at int_no again: saved CS is at rsp+24. Swap GS back if returning to
    ; ring 3 (mirror of the entry test) BEFORE dropping int_no/err and iretq.
    test byte [rsp + 24], 3
    jz .from_kernel_out
    swapgs
.from_kernel_out:

    add rsp, 16           ; discard int_no + err_code
    iretq
