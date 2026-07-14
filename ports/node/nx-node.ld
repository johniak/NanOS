/* nx-node.ld — the toolchain default nx.ld (fixed base 0x800000, flat, .nxheader first) with a
 * contiguous, bracketed C++ constructor collection (.nx_ctors) added. The `make node` target stages
 * this over the sysroot's nx.ld so the node/V8 link uses it as the DEFAULT linker script (NOT via
 * -Wl,-T: ld places the collected .init_array/.ctors differently for a -T script than for the default
 * script — a -T copy resets the section's LMA to the base and overlaps .nxheader, the default-script
 * path does not). Stock nx.ld has no ctor rule, so node's ctors land as orphan sections and crt0's
 * __nx_{init_array,ctors}_{start,end} stay weak-undef=0 → NO static initializer runs
 * (per_process::system_environment stays null → #PF in bootstrap). crt0 runs .init_array forward and
 * legacy .ctors in reverse (the ABI orders). */
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
  /* C++ static constructors, contiguous + bracketed (stock nx.ld has no ctor rule → node's ctors
   * land unbracketed and crt0 runs none → V8 static-init globals stay null → #PF in bootstrap).
   * crt0 runs .init_array forward and legacy .ctors in reverse (the ABI orders). */
  .nx_ctors  : {
    . = ALIGN(8);
    __nx_init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __nx_init_array_end = .;
    . = ALIGN(8);
    __nx_ctors_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(SORT(.ctors.*)))
    KEEP(*(.ctors))
    __nx_ctors_end = .;
  }
  /* Import-library IAT slots, one output section per source library so the name (.nxlib.<soname>)
   * survives — mknx reads it to tag each import with its library. */
  .nxlib.libc.ndl  : { *(.nxlib.libc.ndl) }
  /* TLS template. .tdata/.tbss are not caught by the .data/.bss globs; place them explicitly (and
   * ADJACENT — ld requires it) and export the template bounds for the runtime TLS builder (nx_tls.c),
   * since the flat .nx has no ELF phdrs for musl's auxv discovery. Without this, node's main thread
   * falls to the bare-TCB path: every thread_local (V8 uses many) resolves wrong AND
   * self->robust_list.head stays 0, so the first std::mutex lock #PFs at -0x8(NULL) (cr2=-8) in
   * __pthread_mutex_trylock_owner. Mirrors arch/x86_64/user-nx.ld. */
  .tdata     : { __nx_tls_image = .; *(.tdata .tdata.* .gnu.linkonce.td.*) }
  __nx_tls_edata = .;
  __bss_start = .;
  .tbss (NOLOAD) : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }
  __nx_tls_end = .;
  /* Sizes as ABSOLUTE base-independent constants (crt0 loads them as immediates). Use SIZEOF(.tbss),
   * NOT (__nx_tls_end - __nx_tls_image): `.` does not advance through NOLOAD .tbss, which would drop
   * the whole .tbss and under-size the runtime TLS block. */
  __nx_tls_filesz    = ABSOLUTE(SIZEOF(.tdata));
  __nx_tls_memsz     = ABSOLUTE(SIZEOF(.tdata) + SIZEOF(.tbss));
  __nx_tls_align_sym = ABSOLUTE(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
  .bss       : { *(.bss*) *(COMMON) }
  __bss_end = .;
  __nx_image_size = __bss_start - 0x800000;
}
