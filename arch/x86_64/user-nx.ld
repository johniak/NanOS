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
  /* TLS template. .tdata/.tbss are not caught by the .data/.bss globs, so place them explicitly
     and export the template bounds for the runtime TLS builder (nx_tls.c); the flat .nx has no ELF
     phdrs so musl auxv discovery cannot run. .tdata (init image) is in the loaded image before
     __bss_start; .tbss is NOBITS in the zeroed bss range. __nx_tls_align_sym is an ABSOLUTE symbol
     whose value is the segment alignment (read via (size_t)&__nx_tls_align_sym). */
  .tdata     : { __nx_tls_image = .; *(.tdata .tdata.* .gnu.linkonce.td.*) }
  __nx_tls_edata = .;
  __bss_start = .;
  .tbss (NOLOAD) : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }
  __nx_tls_end = .;
  .bss       : { *(.bss*) *(COMMON) }
  __bss_end = .;
  /* Sizes as ABSOLUTE (base-independent) constants so crt0 loads them as plain immediates — never
     as a runtime difference of two relocated addresses, which mknx could rebase inconsistently when
     a program loads at a base other than 0x800000. Only __nx_tls_image is a real (rebased) address
     (the memcpy source); memsz/filesz/align are constants.
     Use SIZEOF(.tbss), NOT (__nx_tls_end - __nx_tls_image): the location counter `.` does not advance
     through a NOBITS/NOLOAD .tbss, so the subtraction would drop the entire .tbss (all zero-init
     __thread vars — e.g. Mesa's _egl_TLS/_glapi_tls_Context) and report memsz == filesz. That under-
     sizes the runtime TLS block so every var past .tdata lands in unallocated memory. */
  __nx_tls_filesz   = ABSOLUTE(SIZEOF(.tdata));
  __nx_tls_memsz    = ABSOLUTE(SIZEOF(.tdata) + SIZEOF(.tbss));
  __nx_tls_align_sym = ABSOLUTE(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
  /* Stored image size (everything before bss); used as NxHeader.imageSize. */
  __nx_image_size = __bss_start - 0x800000;
}
