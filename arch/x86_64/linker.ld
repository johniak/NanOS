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
  /* The kernel image (incl. .bss) MUST end below VA_USER_BASE (0x800000): per-process address
   * spaces privatize [0x800000, 0x4000000) and load the program image AT 0x800000, so a kernel
   * global past that boundary is unmapped (or aliased onto the user image!) whenever a user CR3
   * is active — IRQ/syscall-context accesses then #PF or corrupt the process. Caught live once:
   * a 512 KiB ProcTable growth pushed g_shootPending to 0x855020 and the shootdown IPI faulted.
   * Fail the LINK, not the boot. */
  ASSERT(_end <= 0x800000, "kernel image + .bss overflow VA_USER_BASE (0x800000) — shrink static tables (see ProcTable arrays) or move them to the heap")
}
