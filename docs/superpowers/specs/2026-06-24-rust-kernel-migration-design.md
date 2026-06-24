# Rust kernel migration — design

**Date:** 2026-06-24
**Status:** approved design, pre-implementation
**Goal:** long-term modernization — the future of the NanOS kernel is Rust, but the
kernel **must boot at every commit**. No big-bang rewrite, no greenfield kernel beside
the live one. Incremental, in-tree coexistence: Rust compiled and linked into the same
`kernel.bin`, behind a stable C ABI, subsystem by subsystem.

This document covers (1) the migration strategy, (2) the **foundation** that must be built
correctly first (no shortcuts), and (3) the **first consumer** — `FrameAllocator` — that
proves the foundation. It is the **template** every later subsystem reuses.

---

## 1. Strategy & non-goals

**Why incremental coexistence (not rewrite / not greenfield).** A ~32k-line kernel that
already does SMP (4 cores), TCP/IP, USB, TLS, ext4 read+write/JBD2, and boots on real
hardware cannot be paused for a multi-month rewrite, and a parallel greenfield kernel is
not "the same kernel that keeps booting." The only path consistent with the goal is:
Rust links into the existing image, the C++↔Rust boundary is a stable C ABI, and each
subsystem flips when its Rust replacement is proven at parity.

**Reference model.** Rust-for-Linux. We adopt its principles (infra-first; `unsafe`
confined to a thin layer behind safe abstractions; *wrap* proven C synchronization
primitives rather than reimplement them; generate FFI; CI discipline) and explicitly do
**not** copy the bulk of its `kernel` crate. Pull principles, not mass — YAGNI.

**Architecture scope.** x86_64 only. i686 is frozen (no Rust on i686). This lets us use
the built-in `x86_64-unknown-none` target on **stable** Rust (precompiled `core`+`alloc`,
no nightly, reproducible).

**Non-goals (for the pioneer):** porting any second subsystem; reimplementing the
spinlock algorithm in Rust; touching i686; any external crates.io runtime dependency in
the kernel.

---

## 2. The foundation — `rust/nanos-kernel` (built correctly, no shortcuts)

A single foundation crate every subsystem crate depends on. `#![no_std]`,
`crate-type = ["rlib"]` (consumed by sibling crates; the *subsystem* crates are the
`staticlib`s that link into the kernel). All `unsafe` FFI is confined to one module;
everything above it is safe Rust.

### 2.1 Panic handler — routes to the kernel, never silent
`#[panic_handler]` formats the panic message + `Location` into a stack buffer and calls
`extern "C" nanos_panic(ptr, len)` (kernel side: print to console + halt, same path the
`Heap` corruption hook uses). Then `loop` on `cpuHalt`. **Not** a bare `loop {}` — a
panic must surface a located message, exactly like a C++ kernel panic.
`panic = "abort"` in the profile (no unwinding; no landing pads in `core`).

### 2.2 Global allocator — correct alignment, not a naive forward
`#[global_allocator]` implementing `GlobalAlloc` over the kernel `Heap`:
- The kernel `Heap.alloc(size)` returns **8-byte-aligned** payloads. Rust `Layout` may
  request align 16/32/64+. A naive `nanos_kmalloc(size)` forward **corrupts** over-aligned
  allocations. **Required, no shortcut:** alignment is honored for every `Layout`.
- Mechanism: `extern "C" nanos_kmalloc_aligned(size, align)` / `nanos_kfree_aligned(ptr,
  size, align)` shims on the kernel side that satisfy `align` (over-allocate + align the
  returned pointer, storing the base for free), with the common 8/16-align path cheap.
  `alloc_zeroed` and `realloc` are implemented (not left to the slow default where a
  cheaper heap path exists). Decision on shim-side vs Heap-side alignment is finalized in
  the plan; either way correctness for `align > 8` is mandatory and unit-tested.
- Host-test build hooks the same trait to libc `aligned_alloc`/`free` so `alloc`-using
  logic is testable off-target.

### 2.3 Locking — wrap the proven primitive, preserve every semantic
We do **not** reimplement the ticket lock in Rust. The C++ `kernel::Spinlock` ticket lock
already (a) is FIFO-fair, (b) services TLB shootdowns inside its spin loop via
`arch::smpPollShootdown()`, and (c) issues the PAUSE hint. Reimplementing risks a rare,
smoke-test-invisible shootdown deadlock.

- `extern "C"` shims expose the existing primitive: `nanos_spin_new`, `nanos_spin_lock`,
  `nanos_spin_unlock` (the spin loop with shootdown-poll stays in C++), plus
  `cpuIrqSave/cpuIrqRestore` for the IRQ-save variant.
- Rust `SpinLock<T>`: owns a `T` in an `UnsafeCell`, holds an opaque handle to the C++
  `Spinlock`. `lock()` returns an RAII `SpinLockGuard<'_, T>` giving `&mut T`; `Drop`
  unlocks. `IrqSpinLock<T>` / `IrqSpinLockGuard` additionally save+cli on acquire and
  unlock-then-restore on drop — **exact** ordering of `SpinIrqGuard` (save+cli → lock;
  unlock → restore). `unsafe impl Sync for SpinLock<T> where T: Send`.
- This gives the SMP payoff the whole effort targets: the type system makes "touch the
  data without holding the lock" **not compile**, while the runtime behavior is the proven
  C++ primitive.

### 2.4 Arch FFI surface (one `unsafe` module)
Declared once, ABI-matched: `cpuIrqSave() -> usize`, `cpuIrqRestore(flags)`, `cpuRelax()`,
`cpuHalt()`, `smpThisCpu() -> i32`, `smpPollShootdown()` (already inside the spin shim),
`nanos_panic(ptr,len)`, `nanos_kmalloc_aligned/kfree_aligned`. No other `unsafe` outside
this module + the `GlobalAlloc`/`SpinLock` internals.

---

## 3. First consumer — `rust/nanos-mm` (FrameAllocator)

`crate-type = ["staticlib"]`, depends on `nanos-kernel`. Replaces the *logic* of
`mm/FrameAllocator.cpp`; the global `kernel::g_frames` symbol and all call sites
(`g_frames.alloc()` …) stay unchanged.

- **State:** `static FRAMES: SpinLock<FrameBitmap>` (the lock from §2.3 owns the bitmap;
  no `static mut`). The 512 KiB bitmap is the crate's `.bss` — equivalent to today's
  `g_frames` in kernel `.bss`. Because the lock is now Rust's, the C++ facade no longer
  takes `SpinIrqGuard` — locking moved into Rust (this is the deliberate step-1 use of the
  lock abstraction, per "full pro").
- **Logic:** `init/clamp`, `mark_free` (round up base, round down end), `mark_used`
  (round down base, round up end), `alloc` (first-free), `free`, `is_used`, `count`. Pure,
  safe Rust over the bitmap. 64-bit addresses preserved (>4 GiB frames keep high bits).
- **C ABI export:** `#[no_mangle] extern "C"` functions `nanos_frame_init`,
  `nanos_frame_mark_free`, `nanos_frame_mark_used`, `nanos_frame_alloc`,
  `nanos_frame_free`, `nanos_frame_is_used`, `nanos_frame_count`. The C header
  `nanos_mm.h` is **generated by cbindgen** (committed, CI-verified in sync — see §5).
- **C++ facade:** `mm/FrameAllocator.cpp` methods become one-line forwarders to the
  exports; the `Spinlock` member is removed (lock lives in Rust). The class + `g_frames`
  remain so callers are untouched. (Facade may later be deleted entirely by switching call
  sites to the C functions — out of scope for the pioneer.)

---

## 4. Build integration

- **Workspace:** top-level `rust/` cargo workspace (sibling to `user/rust/`), members
  `nanos-kernel`, `nanos-mm`. `rust-toolchain.toml` pins an exact stable version +
  `x86_64-unknown-none` target. No runtime crates.io deps; build-time tools (cbindgen)
  pinned by exact version.
- **Docker (`docker/Dockerfile`, `nanos-build`):** install `rustup` + the pinned stable
  toolchain + `rustup target add x86_64-unknown-none` + pinned `cbindgen`, in a **trailing
  layer** (like `lcov`) so it never invalidates the cross-toolchain cache.
- **Makefile:** a rule runs `cargo build --release --target x86_64-unknown-none` in the
  container, producing `$(KOBJ)libnanos_mm.a` (and its dep `nanos-kernel`). The x64 link
  line gains the staticlib(s):
  `$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(RUST_STATICLIBS) -lgcc`. Gated on `ARCH=x86_64`
  (i686 frozen). `make clean` removes the cargo `target/` + generated header.
- **Profile:** `panic = "abort"`, `opt-level` matched to kernel (size/speed), `lto` as
  measured. Target `x86_64-unknown-none` already sets soft-float / `-mno-red-zone`.

---

## 5. Testing & CI (parity, no token tests)

- **Oracle:** the existing `tests/test_*` doctest cases for `FrameAllocator` define the
  contract. Every case is mirrored as a Rust `#[test]` (host build): init/clamp,
  `mark_free`/`mark_used` rounding, first-free `alloc`, `free`, OOM (`alloc` → 0),
  16 GiB cap, addresses >4 GiB. Plus property tests where cheap (alloc/free invariants).
- **End-to-end:** after the flip, the C++ doctest still runs against the C++ facade, which
  now calls the Rust logic through the C ABI — double coverage of the same contract.
- **CI gates wired into `make test`:** `cargo test` (host), `cargo clippy -- -D warnings`,
  `cargo fmt --check`, **`cargo miri test`** over the `unsafe` (FFI/`SpinLock`/`GlobalAlloc`
  modules) to catch UB. cbindgen header checked in sync: regenerate and `diff` against the
  committed `nanos_mm.h`; fail on drift.
- **Coverage gate (>90%) stays green:** the thin C++ facade is trivial; Rust coverage is
  measured by cargo's own tooling. Facade excluded from the C++ `COV_PATTERNS` if needed.

---

## 6. Boot verification

`FrameAllocator` is on the critical paging path. After the flip:
`make image64 && make run64` headless to the login screen via the existing **smoke64**
harness. If frame allocation is wrong, paging triple-faults immediately — a fast,
unambiguous signal. The existing SMP race gates (**smptorture**, **nettorture**) exercise
the new Rust lock under concurrent load — the primary defense for the §7 risk.

---

## 7. The one real risk

The lock/IRQ/shootdown FFI is the hardest part of step one; a mistake there is a rare,
SMP-only deadlock that a single-pass smoke test won't catch. **Mitigations:** (a) we
*wrap* the proven C++ primitive rather than reimplement it — the shootdown-poll spin loop
stays in C++; (b) `smptorture`/`nettorture` run the Rust lock under load; (c) `miri`
checks the `unsafe`. This is why the lock abstraction is built and exercised on the
pioneer, not deferred.

---

## 8. This is the template

The pioneer establishes, once and correctly: the `rust/` workspace + toolchain pin, the
`nanos-kernel` foundation (panic, aligned `GlobalAlloc`, `SpinLock`/`IrqSpinLock` wrapping
the C++ primitive, arch FFI module), the cbindgen header convention, the Docker trailing
layer, the Makefile staticlib rule, and the test-parity + clippy/fmt/miri CI recipe. Every
later subsystem is a sibling `staticlib` crate reusing all of it.

**Tentative migration order after the pioneer** (most isolated → most entangled; revisit
per measured SMP race pressure): `mm/Heap` → `lib` (String/List) → leaf drivers
(keyboard, console sink) → `fs` core → `net`. `net` is where SMP data races bite hardest,
so it is the eventual high-value target, but it migrates last (largest surface, needs the
pattern fully proven first).
