/* Linker script for NanOS user programs (.nx). Fixed base 0x400000; the .nx
 * header must come first so the loader can read it at loadBase. objcopy -O binary
 * of the resulting ELF yields the .nx file directly. */
ENTRY(_start)
SECTIONS
{
  . = 0x400000;
  .nxheader : { *(.nxheader) }
  .text     : { *(.text*) }
  .nximports : { *(.nximports) }
  .rodata   : { *(.rodata*) }
  .data     : { *(.data*) }
  /* Import-library IAT slots, kept as a SEPARATE output section PER source library so the
   * section name (.nxlib.<soname>) survives linking — mknx reads it to tag each import with
   * its library (per-DLL namespace). Kept inside the loaded image so the loader can patch
   * the slots. Add a line here when introducing a new shared library. */
  .nxlib.libc.ndl  : { *(.nxlib.libc.ndl) }
  .nxlib.greet.ndl : { *(.nxlib.greet.ndl) }
  __bss_start = .;
  .bss      : { *(.bss*) *(COMMON) }
  __bss_end = .;
  /* Stored image size (everything before bss); used as NxHeader.imageSize. */
  __nx_image_size = __bss_start - 0x400000;
}
