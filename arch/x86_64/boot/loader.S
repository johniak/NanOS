; arch/x86_64/boot/loader.S — Multiboot1 entry + 32->64 long-mode trampoline (NASM, elf64).
; GRUB/QEMU enter here in 32-bit protected mode (multiboot). We build a temporary
; identity page map (1 GiB via 2 MiB huge pages), enable PAE + EFER.LME + paging, load a
; 64-bit GDT, far-jump into long mode, then call the 64-bit C entry kentry64(mb_info).

MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
VIDEO    equ 1 << 2                 ; request a graphics video mode (like Linux vesafb)
MBFLAGS  equ MBALIGN | MEMINFO | VIDEO
MBMAGIC  equ 0x1BADB002
MBCHECK  equ -(MBMAGIC + MBFLAGS)

section .multiboot
align 4
    dd MBMAGIC
    dd MBFLAGS
    dd MBCHECK
    ; AOUT-kludge fields (offsets 12..28). Unused (flag bit 16 is clear) but must be
    ; present as zeros so the graphics fields land at their fixed offset 32. Mirrors the
    ; i686 loader.s header layout.
    dd 0                            ; header_addr
    dd 0                            ; load_addr
    dd 0                            ; load_end_addr
    dd 0                            ; bss_end_addr
    dd 0                            ; entry_addr
    ; Graphics request (flag bit 2), offsets 32..44. mode_type 0 = linear framebuffer.
    ; width/height = 0 means "no preference" (Multiboot1 §3.1.6): the bootloader keeps the
    ; firmware's current GOP mode, i.e. the panel's NATIVE resolution on real UEFI hardware
    ; (the Dell), instead of being forced to 1024x768. We still prefer 32bpp. The kernel reads
    ; the actual width/height/pitch/bpp back from the Multiboot info (bootinfo_x86_64.cpp), so it
    ; adapts to whatever mode is set.
    dd 0                            ; mode_type = 0 (linear graphics)
    dd 0                            ; width  = no preference (native)
    dd 0                            ; height = no preference (native)
    dd 32                           ; depth (bits per pixel) — prefer 32bpp

section .bss
align 16
stack_bottom:
    ; 1 MiB kernel stack. The 16 KiB it used to be overflowed: the MI ext driver's
    ; mount/read/write paths recurse (extent/indirect-block trees, path resolution) with
    ; several 4 KiB block buffers live per frame, and 64-bit frames are larger than i686's,
    ; so the chain blew past 16 KiB and ran into .data (clobbering the VGA `videoram`
    ; pointer just below stack_bottom -> silent output loss, or a return address -> hang).
    ; The i686 loader reserves ~1.5 MiB for the same reason; match that headroom here.
    resb 1048576
stack_top:
align 4096
p4_table:                       ; PML4
    resb 4096
p3_table:                       ; PDPT (64 entries used -> 64 PDs)
    resb 4096
p2_tables:                      ; 64 contiguous PDs = 64 * 512 * 2 MiB = 64 GiB identity.
    resb 4096 * 64              ; the loader's 1 GiB map was too small: on real x86_64 firmware
                                ; (Dell) the Multiboot info / E820 mmap / framebuffer can sit
                                ; above 1 GiB, and initPaging reads the mmap UNDER this map
                                ; (before mmuInitKernel swaps CR3) -> a >1 GiB access triple-
                                ; faulted with no output. 64 GiB covers any laptop's RAM+MMIO.

section .text
bits 32
global loader
extern kentry64
loader:
    mov esp, stack_top
    mov edi, ebx                ; preserve multiboot info ptr -> becomes rdi (arg0) in long mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp  gdt64.code:long_mode_start   ; far jump activates 64-bit mode

setup_page_tables:
    mov eax, p3_table
    or  eax, 0b11               ; present | writable
    mov [p4_table], eax
    ; Point the first 64 PDPT entries at the 64 contiguous PD tables.
    xor ecx, ecx
.set_p3:
    mov eax, ecx
    shl eax, 12                 ; eax = ecx * 4096 (per-PD offset)
    add eax, p2_tables
    or  eax, 0b11               ; present | writable
    mov [p3_table + ecx * 8], eax
    inc ecx
    cmp ecx, 64
    jne .set_p3
    ; Fill 64 * 512 = 32768 PD entries with 2 MiB huge pages -> identity-map [0, 64 GiB).
    xor ecx, ecx                ; ecx = 2 MiB page index 0..32767
.map_p2:
    mov eax, 0x200000           ; 2 MiB
    mul ecx                     ; edx:eax = 2 MiB * ecx (64-bit; > 4 GiB past index 2047)
    or  eax, 0b10000011         ; present | writable | huge (PS bit 7)
    mov [p2_tables + ecx * 8], eax
    mov [p2_tables + ecx * 8 + 4], edx   ; high phys bits (needed beyond 4 GiB)
    inc ecx
    cmp ecx, 32768
    jne .map_p2
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax                ; CR3 = PML4 phys
    mov eax, cr4
    or  eax, 1 << 5             ; CR4.PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; IA32_EFER MSR
    rdmsr
    or  eax, 1 << 8             ; EFER.LME (long mode enable)
    wrmsr
    mov eax, cr0
    or  eax, 1 << 31            ; CR0.PG (paging) -> activates long mode
    mov cr0, eax
    ret

bits 64
long_mode_start:
    mov ax, 0                   ; null the data segment regs (ignored in long mode)
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top
    call kentry64               ; rdi already holds the multiboot info ptr (from edi)
.hang:
    hlt
    jmp .hang

section .rodata
gdt64:
    dq 0                                            ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)        ; exec | type | present | long-mode (L)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
