# setting up the Multiboot header - see GRUB docs for details
.set ALIGN,    1<<0                     # align loaded modules on page boundaries
.set MEMINFO,  1<<1                     # provide memory map
.set VIDEO,    1<<2                     # request a graphics video mode (like Linux vesafb)
.set FLAGS,    ALIGN | MEMINFO | VIDEO  # this is the Multiboot 'flag' field
.set MAGIC,    0x1BADB002               # 'magic number' lets bootloader find the header
.set CHECKSUM, -(MAGIC + FLAGS)         # checksum required

# The header lives in its own section so the linker script can place it at the
# very start of the image, independent of object link order (GRUB scans only the
# first 8 KiB for the magic).
.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
# Address fields (offsets 12..28). Flag bit 16 is unset so GRUB ignores their
# values, but the graphics fields below are positional, so these 5 dwords must
# still be present to push them to offset 32.
.long 0                                 # header_addr
.long 0                                 # load_addr
.long 0                                 # load_end_addr
.long 0                                 # bss_end_addr
.long 0                                 # entry_addr
# Graphics request (flag bit 2), offsets 32..44. mode_type 0 = linear framebuffer.
# GRUB sets this mode (VBE on BIOS, GOP on UEFI) and reports the framebuffer in the
# Multiboot info. Width/height/depth are a preference; we read back the real values.
.long 0                                 # mode_type = 0 (linear graphics)
.long 1024                              # width
.long 768                               # height
.long 32                                # depth (bits per pixel)

.text
.global loader                          # making entry point visible to linker

# reserve initial kernel stack space
stack_bottom:
.skip 1638400                             # reserve 16 KiB stack
stack_top:
.comm  mbd, 4                           # we will use this in kmain
.comm  magic, 4                         # we will use this in kmain

loader:
    movl  $stack_top, %esp               # set up the stack, stacks grow downwards
    movl  %eax, magic                   # Multiboot magic number
    movl  %ebx, mbd                     # Multiboot data structure

    call  kmain                         # call kernel proper

    cli
hang:
    hlt                                 # halt machine should kernel return
    jmp   hang


.global idt_load
idt_load:
	mov 4(%esp),%eax
	lidt (%eax)
	sti
ret

.global gdt_flush    

gdt_flush:
   mov 4(%esp),%eax  
   lgdt (%eax)        

   mov  $0x10,%ax      
   mov %ax,%ds        
   mov %ax, %es
   mov %ax, %fs 
   mov %ax, %gs 
   mov %ax, %ss 
   jmp $0x08,$flush     
flush:
   ret





