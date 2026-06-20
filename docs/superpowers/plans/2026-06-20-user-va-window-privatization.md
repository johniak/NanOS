# Per-process privatization of all user VA windows — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Privatize EVERY user VA window (program image + module/heap/mmap/fb) per process so the
kernel's >1 GiB huge-page identity map no longer collides with user windows — letting NanOS boot and
use all RAM on a >1 GiB machine, with no userland change.

**Architecture:** The per-process `AddressSpace` already privatizes (`dropPde`) the `[VA_USER_BASE,
VA_USER_END)` window; extend `mmuCreate/Copy/FreeAddressSpace` to cover the module/heap/mmap/fb band
(pdpt[1]) too, via shared file-static helpers. `dropPde` already copies a PD verbatim (even huge PS
entries) and zeroes the window slot in the private copy — so a leaked huge identity entry becomes a
private, empty, 4 KiB-mappable window. Spec: `docs/superpowers/specs/2026-06-20-user-va-window-privatization-design.md`.

**Tech Stack:** C++ freestanding x86_64; `AddressSpace` privatization is host-tested (doctest,
`make ARCH=x86_64 test`, ≥90% lcov); QEMU `-m 6144` for the big-RAM boot smoke.

**Conventions:** MI/MD split (`make check-arch` stays clean). PD stride `PD_SPAN` = 2 MiB. PTE flags in
`arch/x86_64/mm/Paging.h`. Build in Docker; QEMU native.

---

## File Structure (modified)

- `arch/include/arch/mmu.h` — add `VA_FB_MAX` (bounds the framebuffer window for enumeration).
- `arch/x86_64/mm/mmu_x86_64.cpp` — `dropUserWindows()` / `freeUserWindowsAll()` helpers; call them in
  `mmuCreateAddressSpace`, `mmuCopyAddressSpace`, `mmuFreeAddressSpace`; add the fb copy loop in copy.
- `tests/test_addressspace64.cpp` — a test that `dropPde` over a 2 MiB huge identity entry yields a
  private, empty, 4 KiB-mappable window and leaves the source (kernel) directory unchanged.
- `Makefile` — wire `smoke-bigmem` into `verify64`.

---

## Task 1: Host-test — dropPde privatizes a huge identity window without corrupting the kernel dir

**Files:**
- Test: `tests/test_addressspace64.cpp`

This validates the core mechanism the fix relies on (the §6 risk): privatizing a PD that holds a 2 MiB
huge (PS) identity entry, then dropping + 4 KiB-mapping that window, must not touch the source dir.

- [ ] **Step 1: Write the test** (append to `tests/test_addressspace64.cpp`).

```cpp
TEST_CASE("dropPde over a huge identity entry: private + mappable, kernel dir untouched") {
    FakeMem* m = makeMem();
    AddressSpace kern(envOf(m));
    const uint64_t MODVA = 0x40000000;                       // a module-window VA (1 GiB), pdpt[1]
    // Simulate the >1 GiB kernel identity map: a 2 MiB huge page covering the module-window VA.
    REQUIRE(kern.mapRangeHuge(MODVA, MODVA, 0x200000, PTE_PRESENT | PTE_RW));
    CHECK(kern.translate(MODVA) == MODVA);

    // Build a user space like mmuCreateAddressSpace does: share the kernel half, privatize a window,
    // then drop the module window (the new behaviour the fix adds for pdpt[1]).
    AddressSpace proc(envOf(m));
    proc.adoptKernelDirectory(kern.directoryPhys(), 0x800000);   // privatize the program window
    proc.dropPde(MODVA);                                          // privatize + clear the module window

    CHECK(proc.translate(MODVA) == NOPE);                    // user: module window now empty (not huge id)
    REQUIRE(proc.map(MODVA, 0xAB000, PTE_PRESENT | PTE_RW | PTE_USER));   // 4 KiB user map succeeds
    CHECK(proc.translate(MODVA) == 0xAB000u);                // resolves to the user frame
    CHECK(kern.translate(MODVA) == MODVA);                   // kernel dir's huge identity is UNCHANGED
}
```

- [ ] **Step 2: Run it.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "huge identity entry|addressspace|fail"`
Expected: PASS. (`dropPde` + `privatizeChild` already copy the PD verbatim and zero the slot in the
private copy, so this confirms the mechanism is correct for the new pdpt[1] use. If it FAILS, the fix's
premise is wrong and we stop to investigate before touching the kernel glue.)

- [ ] **Step 3: Commit.**

```bash
git add tests/test_addressspace64.cpp
git commit -m "test(mm,x86_64): dropPde privatizes a huge-identity window without corrupting the source dir"
```

---

## Task 2: Privatize/free all user windows in the per-process glue + bound the fb window

**Files:**
- Modify: `arch/include/arch/mmu.h`, `arch/x86_64/mm/mmu_x86_64.cpp`, `Makefile`

- [ ] **Step 1: Add `VA_FB_MAX` to `arch/include/arch/mmu.h`.** After the `VA_FB_BASE` line:

```cpp
constexpr uint64_t VA_FB_MAX        = VA_FB_BASE + 0x4000000;   // +64 MiB (bounds the fb window for enum)
```

- [ ] **Step 2: Add the window helpers in `arch/x86_64/mm/mmu_x86_64.cpp`.** Place them above
  `mmuCreateAddressSpace` (anonymous-namespace file scope is fine; they take an `AddressSpace*`). These
  centralize the window list so create/copy/free can't drift.

```cpp
// Privatize (per-process) EVERY user window beyond the program-image window that adoptKernelDirectory
// already dropped: module/heap/mmap/fb all live in pdpt[1], where the >1 GiB kernel identity map would
// otherwise leak its huge entries into the shared half. dropPde forks pdpt[1]'s PD once (idempotent)
// and clears each window slot in the private copy.
static void dropUserWindows(AddressSpace* s) {
	for (uint64_t va = VA_USER_BASE + PD_SPAN; va < VA_USER_END;   va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_MODULE_BASE;          va < VA_MODULE_MAX; va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_HEAP_BASE;            va < VA_HEAP_MAX;   va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_MMAP_BASE;            va < VA_MMAP_MAX;   va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_FB_BASE;              va < VA_FB_MAX;     va += PD_SPAN) s->impl.dropPde(va);
}

// Free every user window's leaf PT + pages (the private PDPTs/PDs are freed afterwards by freeUserTables).
static void freeUserWindowsAll(AddressSpace* s) {
	for (uint64_t va = VA_USER_BASE;   va < VA_USER_END;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_MODULE_BASE; va < VA_MODULE_MAX; va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_HEAP_BASE;   va < VA_HEAP_MAX;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_MMAP_BASE;   va < VA_MMAP_MAX;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_FB_BASE;     va < VA_FB_MAX;     va += PD_SPAN) s->impl.freeUserWindow(va);
}
```

- [ ] **Step 3: Use `dropUserWindows` in `mmuCreateAddressSpace`.** Replace the existing single drop
  loop:
```cpp
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	for (uint64_t va = VA_USER_BASE + PD_SPAN; va < VA_USER_END; va += PD_SPAN)
		s->impl.dropPde(va);
	return s;
```
with:
```cpp
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	dropUserWindows(s);
	return s;
```

- [ ] **Step 4: Use `dropUserWindows` in `mmuCopyAddressSpace` + add the fb copy loop.** Replace its
  drop loop the same way, and add a framebuffer `copyUserWindowFrom` loop alongside the others:
```cpp
	AddressSpace* s = new AddressSpace(g_env);
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	dropUserWindows(s);
	bool ok = true;
	for (uint64_t va = VA_USER_BASE;   ok && va < VA_USER_END;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_HEAP_BASE;   ok && va < VA_HEAP_MAX;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_MODULE_BASE; ok && va < VA_MODULE_MAX; va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_MMAP_BASE;   ok && va < VA_MMAP_MAX;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_FB_BASE;     ok && va < VA_FB_MAX;     va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	if (!ok) {
		mmuFreeAddressSpace(s);
		return 0;
	}
	return s;
```

- [ ] **Step 5: Use `freeUserWindowsAll` in `mmuFreeAddressSpace`.** Replace its four `freeUserWindow`
  loops with the helper (now including the fb window):
```cpp
	freeUserWindowsAll(s);
	s->impl.freeUserTables();   // free the private PDPTs/PDs freeUserWindow leaves behind
	g_fa->free((uint32_t) s->impl.directoryPhys());
	delete s;
```

- [ ] **Step 6: `check-arch` + build + host tests (no regression in fork/adopt/free).**

Run: `make check-arch && make ARCH=x86_64 test 2>&1 | grep -iE "test cases|fail" && make image64 2>&1 | tail -2`
Expected: arch-clean; all host tests pass (the copy/free balance cases stay green — no frame leak from
the extra fb window); image builds.

- [ ] **Step 7: QEMU — big-RAM now boots; small-RAM unaffected.**

```bash
make smoke-bigmem      # -m 6144
make smoke-x86_64      # -m 512 regression
```
Expected: `big-RAM boot smoke: PASS` (shell reached, `/proc/meminfo` MemTotal > 4 GiB, zero faults) and
`x86_64 MD smoke: PASS`.

- [ ] **Step 8: Wire `smoke-bigmem` into `verify64`.** In `Makefile`, restore it to the gate:
```make
verify64: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-usb
	@echo "x86_64 verify: host tests + BIOS + UEFI + big-RAM + live-USB boot smokes all passed."
```
(and remove the temporary "smoke-bigmem is intentionally NOT in verify64" note added when the gap was
found).

- [ ] **Step 9: Commit.**

```bash
git add arch/include/arch/mmu.h arch/x86_64/mm/mmu_x86_64.cpp Makefile
git commit -m "feat(mm,x86_64): privatize all user VA windows per process — boots + uses >1 GiB RAM (smoke-bigmem gated)"
```

---

## Notes for the implementer

- **Why Task 1 may pass immediately:** `dropPde`/`privatizeChild` already handle a huge PD entry (copy
  verbatim, zero the slot in the private copy). Task 1 is a safety net proving that before we rely on it
  for pdpt[1]; a green run is the expected, good outcome. A red run means stop and rethink (the fix's
  premise would be wrong).
- **Fork (`copyUserWindowFrom`) over the now-private fb window:** harmless when the fb window is empty
  (it returns success/no-op); the added loop just preserves a mapped fb across fork, matching the other
  windows.
- **Frame-leak check:** the host suite's copy/free cases assert alloc/free balance — after adding the fb
  window to both drop and free, they must stay green (the private pdpt[1] PD + fb PT are freed by
  `freeUserWindowsAll` + `freeUserTables`).
- **Out of scope:** higher-half kernel, xHCI BIOS handoff (separate deferred task), NVMe, e1000e.
```
