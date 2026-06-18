/* kext64.ld — link script for an x86_64 NanOS kernel module (.nkext). The x86_64 sibling of
 * kext.ld: same section order + ENTRY(nkext_init) + .nxlib.kernel import slots, but emits
 * elf64-x86-64 and links at a FIXED LOW base (< 2 GiB) so the kext's small/kernel-model
 * absolute references satisfy R_X86_64_32S (sign-extended 32-bit). The base is nominal —
 * the loader relocates the module by (kernel-heap buffer - base) at load time. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(nkext_init)
SECTIONS
{
  . = 0x800000;                  /* fixed low base (< 2 GiB) — satisfies R_X86_64_32S */
  .nxheader     : { *(.nxheader) }
  .text         : { *(.text*) }
  .nximports    : { *(.nximports) }
  .rodata       : { *(.rodata*) }
  .data         : { *(.data*) }
  .nxlib.kernel : { *(.nxlib.kernel) }
  __bss_start = .;
  .bss          : { *(.bss*) *(COMMON) }
  __bss_end = .;
}
