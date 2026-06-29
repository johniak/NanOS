---
name: nanos-x64-paging-carryforward
description: x86_64 Plan 3 (4-level paging) carry-forward gates to close before multiprocess/large-RAM
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

x86_64 Plan 3 (4-level PML4 paging) landed on `feat/x86_64-foundation` and boots clean as a single-process staged kernel (own PML4, identity-map RAM + NX, QEMU zero faults). Code review flagged forward-looking gates that are NOT bugs in the exercised single-process path but MUST be closed before enabling multiprocess or RAM > 1 GiB:

1. **~1 GiB RAM ceiling — FIXED 2026-06-20 (commit 5e7bb87, branch `feat/real-hw-boot`).** Two-part fix: (a) `mmuInitKernel` now identity-maps ALL RAM with 2 MiB huge pages (`AddressSpace::mapRangeHuge`, `PTE_PS` in `Paging.h`), and carves the kernel heap at/below `BOOT_IDENTITY`=1 GiB so it's reachable under the loader's temp map; FrameAllocator bitmap is 16 GiB (`CAPACITY_BYTES`), `bootMemTop()`/`highestUsableAddr` are uint64. (b) The user VA windows (`VA_MODULE_BASE`/`VA_HEAP_BASE`/`VA_MMAP_BASE`/`VA_FB_BASE`, 0x40000000–0x58000000 in pdpt[1]) are now PRIVATIZED per process: `mmuCreate/Copy/FreeAddressSpace` call file-static `dropUserWindows()`/`freeUserWindowsAll()` over every window (added `VA_FB_MAX`=`VA_FB_BASE`+64 MiB to bound the fb). `dropPde` copies pdpt[1]'s PD verbatim (even the leaked huge identity entries) then zeroes the window slot in the PRIVATE copy, so each process gets empty 4 KiB-mappable windows while the kernel keeps its identity map. Verified: `make smoke-bigmem` (-m 6144, MemTotal>4 GiB, zero faults) now in `verify64`; -m 512 unaffected. The small-code-model userland (VA<2 GiB) is unchanged — no higher-half kernel.

2. **`adoptKernelDirectory` 4-level privatization — AUDITED + CORRECTED 2026-06-19 (commit 9758df9).** It now also clears the AVL `PTE_PRIV` bit on the shared kernel-half entries so they are re-forked on first user write and never freed by a process teardown. Multiprocess is exercised daily now (bash/nwm/desktop) with zero faults.

3. **Intermediate-table leak — FIXED 2026-06-19 (commit 9758df9), and it was MUCH bigger than the "~2-3 frames" estimated here.** Root cause was not just the missing teardown-free: `privatizeChild` re-allocated an already-private PDPT/PD on EVERY `dropPde` iteration (~27 per fork over the USER window), orphaning the prior copies — **~57 frames / ~232 KB leaked PER PROCESS, linearly** (measured fork alloc 704 vs teardown free 647; MemFree fell 23 MB/100 procs → OOM after ~2200). Fix: a `PTE_PRIV` AVL-bit marker makes `privatizeChild` idempotent (reuse, don't re-copy), `nextTable` marks new private tables, copies clear PTE_PRIV on aliased children, and `mmuFreeAddressSpace`→`freeUserTables()` frees the marked PDPTs/PDs. Re-measured: 650 alloc == 650 free, MemFree flat (−4 KB/100). Verified by `test_addressspace64`/`test_paging64` + the MD boot smoke (`make smoke-x86_64`).

4. **Minor:** tiny-RAM heap-carve underflow in `mmuInitKernel` (if `topOfRam < 8 MiB` the clamp wraps) — shared with i686 `mmu_x86.cpp`; one-line guard. FB window not copied on fork (consistent with i686; fb is remapped per exec).

See [[nanos-x64-migration]]. Plan 4 (interrupts/IDT) supersedes the staged PIC-mask scaffolding in `arch/x86_64/boot/KernelStage64.cpp`.
