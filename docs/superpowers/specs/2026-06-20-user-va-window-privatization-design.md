# Per-process privatization of all user VA windows — design

**Date:** 2026-06-20
**Branch:** `feat/real-hw-boot` (continues the real-HW boot work; x86_64 sole dev arch).
**Status:** design approved, ready for the implementation plan.

---

## 1. Goal & scope

Let the x86_64 kernel **identity-map and use all physical RAM (> 1 GiB)** without the user-space VA
windows colliding with the kernel identity map — so NanOS boots and runs on the Dell's 8–16 GiB. The
fix stays entirely in the per-process address-space setup (`arch/x86_64/mm/AddressSpace.*` +
`mmu_x86_64.cpp`); the userland keeps its small code model (VA < 2 GiB) **unchanged** — no recompile,
no higher-half kernel (consciously rejected as too large for this step).

Out of scope: higher-half kernel, NVMe, the e1000e/I219 NIC, the xHCI BIOS handoff (a separate
deferred task), and any userland/SDK change.

## 2. The defect (precise mechanism)

All user VA windows live in the low 2 GiB (the small code model pins them there): the program image at
`VA_USER_BASE`=8 MiB (pdpt[0]), and the `.ndl` module / brk-heap / mmap / framebuffer band at
`VA_MODULE_BASE`=1 GiB … ~1.4 GiB (pdpt[1]). `mmuInitKernel` now identity-maps **all RAM** with 2 MiB
huge pages. On a machine with > 1 GiB RAM the kernel identity map fills pdpt[1] (VA 1–2 GiB) with huge
(PS) entries — exactly where the module/heap/mmap/fb windows live.

A user `AddressSpace` is built by `adoptKernelDirectory(kernelTop, VA_USER_BASE)` (shares the kernel
PML4 by value, clearing `PTE_PRIV`) plus `dropPde` over **only** `[VA_USER_BASE, VA_USER_END)`
(8–64 MiB, pdpt[0]). The module/heap/mmap/fb windows (pdpt[1]) are therefore left **shared**. On
≤ 1 GiB RAM that was harmless — the identity map never reached pdpt[1], so those windows were unmapped
and freely usable. On > 1 GiB RAM the shared pdpt[1] carries the kernel's **huge identity** entries, so
when the dynamic loader `map()`s a 4 KiB page into a module window, `nextTable` finds the present huge
PD entry and walks into the 2 MiB **data page** as if it were a page table → corruption, and the user
VA still resolves to the supervisor-only identity page → the process faults (the observed
`killed faulting process cr2=0x40008000`).

## 3. The fix

In **`mmuCreateAddressSpace`** and **`mmuCopyAddressSpace`** (`mmu_x86_64.cpp`), extend the per-process
privatization to cover **every** user window, not just `[VA_USER_BASE, VA_USER_END)`. After the existing
`adoptKernelDirectory` + the `[VA_USER_BASE, VA_USER_END)` drop loop, also `dropPde` across:
`[VA_MODULE_BASE, VA_MODULE_MAX)`, `[VA_HEAP_BASE, VA_HEAP_MAX)`, `[VA_MMAP_BASE, VA_MMAP_MAX)`, and the
framebuffer window `[VA_FB_BASE, VA_FB_MAX)` — each stepped by `PD_SPAN` (2 MiB).

`dropPde(va)` already does the right thing for this case: `privatizePdPath` makes a private copy of the
pdpt[1] PD (idempotent across the band — copied once, marked `PTE_PRIV`), and `pd[pdIndex(va)] = 0`
clears the leaked huge identity entry **in the private copy only**. The kernel's own directory keeps
its identity map (for frame access); each user process gets private, empty, 4 KiB-mappable windows. The
dynamic loader / brk / mmap / fb mappers then populate them with `PTE_USER` pages as before.

A single new constant bounds the framebuffer window so it can be enumerated:
`VA_FB_MAX = VA_FB_BASE + 0x4000000` (64 MiB — ample for any firmware framebuffer).

## 4. Teardown (no leak)

`mmuFreeAddressSpace` already frees the USER/HEAP/MODULE/MMAP windows via `freeUserWindow` then
`freeUserTables` (the private PDPTs/PDs marked `PTE_PRIV`). Add the **FB window** to its
`freeUserWindow` loop so the now-privatized fb window's leaf PT + pages are freed too. `freeUserTables`
already frees the extra private pdpt[1] PD generically (it walks `PTE_PRIV` tables), so no other
teardown change is needed.

## 5. Testing

- **Host doctest** (`AddressSpace` privatization is host-tested in `tests/test_addressspace64.cpp`):
  build a fake "kernel" directory that has a **2 MiB huge identity entry at a module-window VA**
  (simulating a > 1 GiB identity map). Create a user `AddressSpace` via the same adopt+drop sequence the
  kernel uses; assert (a) the module-window VA is **not present / not the huge entry** in the user
  space, (b) a `map()` of a 4 KiB user page there succeeds and `translate()` returns the user frame, and
  (c) the original kernel directory's entry is **unchanged** (no shared-table corruption). The existing
  fork/adopt/free cases stay green (no leak: alloc/free balanced).
- **QEMU**: `make smoke-bigmem` (-m 6144, a usable region above the 4 GiB PCI hole) now reaches the
  shell with `/proc/meminfo MemTotal > 4 GiB` and zero faults; wire `smoke-bigmem` into `verify64`.
  `-m 512` (the existing BIOS/UEFI/USB smokes) is unaffected.

## 6. Risks

- One extra private PD (pdpt[1]) per process (~4 KiB), freed at teardown — negligible.
- Privatizing a PD that contains huge (PS) entries: `privatizeChild` copies the 512 entries verbatim
  then `dropPde` zeroes the window slots; the non-window huge entries it copies remain supervisor-only
  (no `PTE_USER`) so ring-3 cannot touch them — covered by the host test. `freeUserTables` frees the
  private PD frame without descending into kernel-aliased entries (already the case).
- The framebuffer window is dynamically sized; `VA_FB_MAX` bounds the enumeration safely (the fb is far
  smaller than 64 MiB).
