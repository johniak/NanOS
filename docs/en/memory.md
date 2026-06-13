# NanOS Memory Management

NanOS runs with **paging enabled** and a **per-process address space** — not the flat,
no-paging model the early CLAUDE.md notes describe. The stack is: a physical **frame allocator**
(bitmap), the **x86 paging** bring-up (identity-mapped kernel + private user windows), a real
**kernel heap** (free-list with coalescing, behind `new`/`malloc`), and the userland windows
(`brk`, `mmap`, the shared-library band). This doc unifies it; the userland VA map also appears
from the program's angle in nxe-ndl.md §4 and the per-process angle in scheduler.md §5.

---

## 1. Physical frame allocator (`mm/FrameAllocator.*`)

A **bitmap** over 4 KiB frames (`FRAME_SIZE = 4096`), one bit per frame, `MAX_FRAMES = 1<<20` →
a 128 KiB bitmap living in `.bss` (no constructor — the global `g_frames` is zeroed by the loader).

- `init(topOfRam)` marks everything used, then `arch::bootMemForEachUsable` (the Multiboot map,
  boot.md §3) calls `markFree` on each usable range. Reserved ranges stay used: low memory
  (<1 MiB), the kernel image, the 4 MiB staging window at `0x800000`, and the kernel heap.
- `alloc()` returns a free frame's physical address (or 0 = OOM); `free(pa)` clears its bit;
  `freeCount()` walks the bitmap for `/proc/meminfo`.
- Pure → host-tested (`test_frameallocator.cpp`).

This is the single source of physical pages — both page tables and user data frames come from here.

---

## 2. Paging bring-up (`arch/x86/mm/mmu_x86.cpp`, `Paging.h`, `PagingControl.h`)

Classic 32-bit x86 2-level paging (10-bit PD index | 10-bit PT index | 12-bit offset), PTE flags
`PRESENT | RW | USER`. The CR-register wrappers are in `PagingControl.h` (`loadCr3`/`readCr3`/
`enablePaging`/`readCr2`).

`mmuInitKernel(frames, topOfRam)`:
1. reserve the frame regions (low mem, kernel, staging, heap);
2. lay the kernel heap arena (`heapInit`, §5) — **before** paging is on, while addresses are
   physical;
3. build the kernel page directory and **identity-map all RAM** `[0, topOfRam)` as `PRESENT|RW`;
4. `cli`, `loadCr3(kernelDir)`, `enablePaging()` (set CR0.PG), `sti`.

`mmuMapKernelMmio(phys, len)` maps device MMIO into the live kernel directory — used at boot to map
the framebuffer before any per-process space exists, so the mapping is shared (terminal.md).

---

## 3. Per-process address space (`arch/x86/mm/AddressSpace.*` + `mmu_x86.cpp`)

Each process gets its own page directory, **cloned from the kernel directory** so all
identity-mapped RAM and the kernel heap are shared; then specific windows are carved out as
**private** page tables:

- `mmuCreateAddressSpace()` → `adoptKernelDirectory(kernelDir, 0x800000)`: copy every PDE from the
  kernel directory, then **zero the PDE covering the user window** (`0x800000`) so the first map
  there allocates a private page table instead of aliasing the staging frame.
- `AddressSpace::map(va, pa, flags)`: on-demand — if the PDE isn't present, allocate a zeroed frame
  for a new page table (`PRESENT|RW|USER`), then set the PTE.
- `mmuSwitch(space)` loads the directory into CR3 (TLB flushes); the scheduler calls it on every
  context switch (scheduler.md §2).
- `mmuCopyAddressSpace(parent)` (**fork**, eager): clone the kernel half, then `copyUserWindowFrom`
  each populated private window (user window, heap, module band, mmap) — fresh frames, bytes copied.
- `mmuFreeAddressSpace(space)` (exit): free only the private parts (window PTs + their frames, the
  directory); shared kernel PDEs and the framebuffer MMIO window are left alone.
- `AddressSpace` is pure → host-tested (`test_addressspace.cpp`).

---

## 4. The full memory map

Constants from `mmu_x86.cpp`. Below `0x10000000` the RAM is identity-mapped in the kernel
directory; a process overrides specific windows with private mappings.

| Region | VA range | Constant / note |
|---|---|---|
| Low memory + VGA | `0` – `0x100000` | reserved |
| Kernel image | `0x100000`+ | load base (`linker.ld`) |
| Identity-mapped RAM | `0` – `topOfRam` | kernel directory (incl. the kernel heap) |
| **Exec staging window** | `0x800000`, 4 MiB | reserved frame; image staged under kernel CR3 (nxe-ndl.md §4) |
| **User window** (private) | `0x800000` – `0xBFFFFF` | program image + 512 KiB stack at the top |
| **Module band** (private) | `0x08000000` – `0x10000000` | `NX_MOD_BASE/STRIDE/MAX` — 4 MiB × 32 `.ndl` slots |
| **Framebuffer** | `0x10000000` | `FB_USER_VA` — MMIO, not freed on exit |
| **Heap / brk** (private) | `0x20000000` – `0x22000000` | `NX_BRK_BASE/MAX` — 32 MiB |
| **mmap** (private) | `0x30000000` – `0x34000000` | `NX_MMAP_BASE/MAX` — 64 MiB |

---

## 5. Kernel heap (`mm/Heap.*` + `mm/memory_manager.*`)

The kernel allocator behind `new`/`malloc` is a **real free-list heap with boundary tags and
coalescing** (`mm/Heap.cpp`) — not a bump allocator, and `free` genuinely frees. (The old
bump allocator with a no-op `free`, mentioned in early CLAUDE.md notes, has been superseded.)

- Each block has an 8-byte header + 8-byte footer (size + used bit); the free list is doubly
  linked by **offsets from the arena base** (so it's host-testable on a 64-bit host).
- `alloc` is first-fit + split; `free` coalesces both neighbours.
- **Integrity:** a `0xC7` canary after each allocation + boundary-tag checks; a mismatch calls
  `onCorruption` → a kernel panic (this is what turns a per-task kernel-stack overflow into a
  clean panic rather than silent corruption — see scheduler.md §1).
- `memory_manager.cpp` is the thin façade: `heapInit(base,size)` lays the arena (called from
  `mmuInitKernel`), and `malloc/calloc/realloc/free` + the C++-linkage `operator new/delete`
  all delegate to the heap. `heapTotalBytes()`/`heapFreeBytes()` feed `/proc/meminfo`.
- Host-tested (`test_memory_manager.cpp`).

> Note the **C++ linkage** of `malloc` etc. (not `extern "C"`) — the host tests provide matching
> shims forwarding to libc, since they don't link `memory_manager` (CLAUDE.md, Testing).

---

## 6. User heap & mmap (`brk` / `mmap`)

The userland heap and mmap windows grow on demand via syscalls (syscalls.md), each
implemented by switching to the kernel directory (the only place a fresh frame is identity-
accessible to zero), mapping frames into the *process* space, then restoring CR3:

- **`brk`/`sbrk`** → `mmuSetUserBrk(space, old, new)`: page-rounds both ends; growing maps fresh
  zeroed `USER|RW` frames in `[old, new)`, shrinking unmaps + frees. glibc-style: returns the new
  break, unchanged on failure. Window: `NX_BRK_BASE … NX_BRK_MAX`.
- **`mmap`** → `mmuMapAnon(space, base, bytes, writable)` for `MAP_ANONYMOUS` (fresh zeroed
  frames) and as the backing store for file-backed maps (the dispatch then fills from the file);
  `mmuMapUserFb` maps the framebuffer MMIO at `FB_USER_VA`. Window: `NX_MMAP_BASE … NX_MMAP_MAX`.

Both windows are duplicated on fork (`copyUserWindowFrom`) and released on exit
(`mmuFreeAddressSpace`).

---

## 7. `/proc/meminfo`

Rendered by `fs/SynthFs.cpp` from the kernel stats (`Kernel.cpp`):

- `MemTotal` = `bootMemTop()/1024`, `MemFree` = `g_frames.freeCount() * 4`,
- `KHeapTotal`/`KHeapFree` = the heap arena size / free payload.

---

## 8. Invariants

- **No global constructors** — rely on zeroed `.bss` + explicit `init()` (the frame-allocator
  bitmap, the heap arena). `.bss` is zeroed by the Multiboot loader.
- **Paging is on** (kernel identity map + per-process windows) — the "single flat address space"
  line in old CLAUDE.md notes is historical.
- **Single CPU**, no TLB shootdown beyond the implicit flush on `loadCr3`.

**Key files:** `mm/{FrameAllocator,Heap,memory_manager}.*`, `arch/x86/mm/{mmu_x86.cpp,
AddressSpace.*, Paging.h, PagingControl.h}`, `kernel/{Kernel.cpp, SyscallDispatch.cpp}`,
`fs/SynthFs.cpp`.
