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
  __bss_start = .;
  .bss      : { *(.bss*) *(COMMON) }
  __bss_end = .;
  /* Stored image size (everything before bss); used as NxHeader.imageSize. */
  __nx_image_size = __bss_start - 0x400000;
}
