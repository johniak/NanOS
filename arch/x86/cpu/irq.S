%macro IRQ 2
  global irq%1
  irq%1:
    cli
    push byte 0
    push byte %2
    jmp irq_common_stub
%endmacro


IRQ   0,    32
IRQ   1,    33
IRQ   2,    34
IRQ   3,    35
IRQ   4,    36
IRQ   5,    37
IRQ   6,    38
IRQ   7,    39
IRQ   8,    40
IRQ   9,    41
IRQ   10,    42
IRQ   11,    43
IRQ   12,    44
IRQ   13,    45
IRQ   14,    46
IRQ   15,    47


[EXTERN irq_handler]
[EXTERN schedPreempt]

irq_common_stub:
   pushad

   mov ax, ds
   push eax

   mov ax, 0x10
   mov ds, ax
   mov es, ax
   mov fs, ax
   mov gs, ax

   call irq_handler

   ; Deferred preemption: switch tasks ONLY on the way back to ring 3, where the kernel
   ; stack holds a full, clean trap frame. The saved CS sits at [esp+48] here (esp+0=ds,
   ; +4..35=pushad, +36=int_no, +40=err_code, +44=eip, +48=cs). If we are returning to
   ; ring 0 (kernel), skip -- the kernel is never preempted at an arbitrary instruction.
   test dword [esp + 48], 3
   jz .no_resched
   call schedPreempt
.no_resched:

   pop ebx
   mov ds, bx
   mov es, bx
   mov fs, bx
   mov gs, bx

  popad
   add esp, 8
   sti
   iret
