; ap_trampoline.S — application-processor real-mode -> long-mode bring-up blob (NASM, elf64).
;
; A SIPI wakes an AP in 16-bit real mode at CS:IP = (vec<<8):0000, i.e. linear AP_PHYS (0x8000).
; This blob is linked into the high kernel image; smpBringUpAPs() memcpy's the bytes in
; [ap_trampoline_start, ap_trampoline_end) down to 0x8000 and patches ap_pml4 / ap_entry64 /
; ap_stack before each SIPI. Every absolute reference is written as (label - ap_trampoline_start
; + AP_PHYS) — a compile-time constant within this one section — so the copied bytes execute
; correctly at 0x8000 regardless of the link address. Keep the blob < 4 KiB.
;
; Path: set DS=0 + a flat 32-bit GDT -> CR0.PE -> 32-bit -> load kernel CR3, CR4.PAE, EFER.LME,
; CR0.PG (long mode) + a 64-bit GDT -> 64-bit -> load the per-AP stack and call apEntry64.

AP_PHYS equ 0x8000

[GLOBAL ap_trampoline_start]
[GLOBAL ap_trampoline_end]
[GLOBAL ap_pml4]
[GLOBAL ap_entry64]
[GLOBAL ap_stack]

section .text
bits 16
ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax                  ; DS=0 so the absolute 0x80xx displacements below resolve flat
    mov es, ax
    mov ss, ax
    lgdt [ap_gdt32_ptr - ap_trampoline_start + AP_PHYS]
    mov eax, cr0
    or  eax, 1                  ; CR0.PE
    mov cr0, eax
    jmp dword 0x08:(ap_pm - ap_trampoline_start + AP_PHYS)   ; flush into 32-bit code

bits 32
ap_pm:
    mov ax, 0x10                ; flat 32-bit data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [ap_pml4 - ap_trampoline_start + AP_PHYS]
    mov cr3, eax                ; the BSP's kernel PML4 (identity-mapped low RAM covers us)
    mov eax, cr4
    or  eax, (1 << 5)           ; CR4.PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or  eax, (1 << 8)           ; EFER.LME
    wrmsr
    mov eax, cr0
    or  eax, (1 << 31)          ; CR0.PG -> long mode active
    mov cr0, eax
    lgdt [ap_gdt64_ptr - ap_trampoline_start + AP_PHYS]
    jmp 0x08:(ap_lm - ap_trampoline_start + AP_PHYS)         ; flush into 64-bit code

bits 64
ap_lm:
    mov rsp, [ap_stack - ap_trampoline_start + AP_PHYS]
    mov rax, [ap_entry64 - ap_trampoline_start + AP_PHYS]
    call rax                    ; apEntry64() — never returns
.hang:
    hlt
    jmp .hang

; --- temporary GDTs (live inside the copied blob, so the lgdt bases are constants) -----------
align 8
ap_gdt32:
    dq 0x0000000000000000       ; null
    dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code, base 0, limit 4 GiB, 4 KiB granular
    dq 0x00CF92000000FFFF       ; 0x10: 32-bit data, base 0, limit 4 GiB, 4 KiB granular
ap_gdt32_end:
ap_gdt32_ptr:
    dw ap_gdt32_end - ap_gdt32 - 1
    dd ap_gdt32 - ap_trampoline_start + AP_PHYS

align 8
ap_gdt64:
    dq 0x0000000000000000                       ; null
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53)          ; 0x08: 64-bit code (exec|type|present|long)
ap_gdt64_end:
ap_gdt64_ptr:
    dw ap_gdt64_end - ap_gdt64 - 1
    dd ap_gdt64 - ap_trampoline_start + AP_PHYS

; --- patch slots (filled by smpBringUpAPs before each SIPI) ----------------------------------
align 8
ap_pml4:    dq 0                ; kernel CR3 (PML4 phys)
ap_entry64: dq 0               ; &apEntry64
ap_stack:   dq 0               ; this AP's kernel stack top
ap_trampoline_end:
