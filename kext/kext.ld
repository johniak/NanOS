/* kext.ld — link script for a NanOS kernel module (.nkext). Like user/dll.ld (relocatable;
 * the loader places the module in a kernel heap buffer at runtime), but with an ENTRY
 * (nkext_init, recorded by mknx) and a .nxlib.kernel section holding the kernel import slots
 * (mknx tags those imports with source library "kernel"). Preferred base is arbitrary —
 * everything is relocated by (buffer - base). */
ENTRY(nkext_init)
SECTIONS
{
  . = 0xD0000000;
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
