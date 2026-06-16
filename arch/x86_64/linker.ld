OUTPUT_FORMAT(elf64-x86-64)
ENTRY(loader)
SECTIONS
{
  . = 0x100000;                 /* GRUB/QEMU load the kernel at 1 MiB, 32-bit PM entry */
  .boot   : { *(.multiboot) }   /* Multiboot header first, within GRUB's 8 KiB scan window */
  .text   : { *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) }
  .data   : { *(.data) *(.data.*) }
  .bss    : { *(.bss) *(.bss.*) *(COMMON) }
  end = .; _end = .; __end = .;
}
