/* Linker script for NanOS x86_64 user programs (.nx). Fixed low base 0x800000 so every
 * absolute reference fits the small/kernel code model and satisfies R_X86_64_32S
 * relocations (sign-extended 32-bit, i.e. the low 2 GiB). The .nxheader must come first
 * so the loader can read it at loadBase; objcopy -O binary of the resulting ELF yields
 * the .nx file directly. Mirrors user/nx.ld (i686) but emits elf64-x86-64. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS
{
  . = 0x800000;                  /* fixed low base (< 2 GiB) — satisfies R_X86_64_32S */
  .nxheader  : { KEEP(*(.nxheader)) }
  .text      : { *(.text*) }
  .nximports : { *(.nximports) }
  .rodata    : { *(.rodata*) }
  .data      : { *(.data*) }
  /* Import-library IAT slots, kept as a SEPARATE output section per source library so the
   * section name (.nxlib.<soname>) survives linking — mknx reads it to tag each import
   * with its library. Add a line here when introducing a new shared library. */
  .nxlib.libc.ndl  : { *(.nxlib.libc.ndl) }
  __bss_start = .;
  .bss       : { *(.bss*) *(COMMON) }
  __bss_end = .;
  /* Stored image size (everything before bss); used as NxHeader.imageSize. */
  __nx_image_size = __bss_start - 0x800000;
}
