; switch.S — x86 context switch + task trampoline for the scheduler.
; NASM (Intel). Symbols are bare (no leading underscore), matching the kernel ABI.
[BITS 32]
[GLOBAL archContextSwitch]
[GLOBAL taskTrampoline]
[EXTERN schedulerRunCurrentBody]

; void archContextSwitch(unsigned* saveOldKesp, unsigned newKesp)
;   Save callee-saved regs + CR3 + esp of the current task into *saveOldKesp, then
;   load the next task's context. Stack after the 5 pushes (top -> bottom):
;     [esp]    cr3
;     [esp+4]  ebp
;     [esp+8]  edi
;     [esp+12] esi
;     [esp+16] ebx
;     [esp+20] return address
;     [esp+24] saveOldKesp (arg1)
;     [esp+28] newKesp     (arg2)
archContextSwitch:
    push ebx
    push esi
    push edi
    push ebp
    mov eax, cr3
    push eax
    mov eax, [esp + 24]      ; saveOldKesp
    mov [eax], esp           ; *saveOldKesp = current esp
    mov esp, [esp + 28]      ; esp = newKesp (read from old stack, then switch)
    pop eax
    mov cr3, eax             ; restore the task's address space (flushes TLB)
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret                      ; resume the task (its preempted frame / trampoline)

; First entry of a freshly bootstrapped task (archTaskBootstrap planted this as the
; "return address"). Enable interrupts so it can be preempted, then run its body.
taskTrampoline:
    sti
    call schedulerRunCurrentBody
.hang:
    hlt
    jmp .hang
