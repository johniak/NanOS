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
  /* Constructor pointer array for THIS module. picolibc registers a few startup
   * constructors here (notably the std-stream lock-init `posix_init`, emitted into the
   * legacy `.ctors`), but NanOS links userland/libc with -nostdlib, so there is no crt
   * that runs them. We collect them with boundary symbols so libc.ndl's own startup
   * (__nx_init_tls, see user/libc-glue/tls.c) can iterate and call them once per process.
   * Both `.init_array` and `.ctors` are collected for robustness; this picolibc uses
   * `.ctors`. No crtstuff sentinel words — the array is raw function pointers, and the
   * runner uses the bracketing symbols rather than a count/terminator. The runner calls
   * them FORWARD; the current ctors (3x std-stream posix_init + __stack_chk_init) are
   * order-independent, so this is fine, but it does NOT reproduce glibc semantics (legacy
   * `.ctors` run in reverse; all `.init_array` here run before all `.ctors`) — do not add
   * an order-dependent / priority-sensitive constructor expecting otherwise.
   * Destructors (`.dtors`/`.fini_array`, e.g. the stream-flush `posix_exit`) are NOT
   * collected: programs exit via the kernel longjmp, not C atexit, so they would not run
   * anyway. */
  .nx_init_array : {
      PROVIDE_HIDDEN(__nx_ctors_start = .);
      KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
      KEEP(*(.init_array))
      KEEP(*(SORT_BY_NAME(.ctors.*)))
      KEEP(*(.ctors))
      PROVIDE_HIDDEN(__nx_ctors_end = .);
  }
  .bss       : { *(.bss*) *(COMMON) }
}
