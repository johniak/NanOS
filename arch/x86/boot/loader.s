# setting up the Multiboot header - see GRUB docs for details
.set ALIGN,    1<<0                     # align loaded modules on page boundaries
.set MEMINFO,  1<<1                     # provide memory map
.set FLAGS,    ALIGN | MEMINFO          # this is the Multiboot 'flag' field
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





