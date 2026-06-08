/* Linker script for NanOS shared libraries (.ndl). Preferred base 0x09000000; the
 * dynamic loader relocates each module to its assigned window in the module band, so the
 * preferred base only has to be a valid, distinct address. The .nxheader placeholder
 * comes first so mknx can stamp the header at the load base; .nximports holds any import
 * descriptors (a .ndl that itself imports from another .ndl). */
SECTIONS
{
  . = 0x09000000;
  .nxheader  : { *(.nxheader) }
  .text      : { *(.text*) }
  .nximports : { *(.nximports) }
  .rodata    : { *(.rodata*) }
  .data      : { *(.data*) }
  .bss       : { *(.bss*) *(COMMON) }
}
