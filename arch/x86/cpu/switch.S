; switch.S — x86 context switch + task trampoline for the scheduler.
; NASM (Intel). Symbols are bare (no leading underscore), matching the kernel ABI.
[BITS 32]
[GLOBAL archContextSwitch]
[GLOBAL taskTrampoline]
[EXTERN schedulerRunCurrentBody]

; void archContextSwitch(uintptr_t* saveOldKesp, uintptr_t newKesp)
;   (uintptr_t == 32-bit on i686, so the body below is unchanged; the C contract is widened)
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
    pop eax                  ; eax = the next task's saved cr3
    mov ecx, cr3             ; ecx = the current (outgoing) cr3
    cmp eax, ecx
    je .samecr3              ; same address space (e.g. kernel thread <-> kernel thread):
    mov cr3, eax             ;   only reload — and flush the TLB — when it actually changes
.samecr3:
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
