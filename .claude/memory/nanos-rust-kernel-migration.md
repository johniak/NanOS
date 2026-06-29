---
name: nanos-rust-kernel-migration
description: "incremental Rust-into-kernel modernization; foundation-first, FrameAllocator pioneer, x86_64-only"
metadata: 
  node_type: memory
  type: project
  originSessionId: 265960ef-8857-4c2d-b77d-6d23ad3adf6e
---

Long-term goal: migrate the NanOS C++ kernel (~32k LOC: kernel/arch/fs/net/drivers/mm)
to Rust **incrementally** — Rust linked into the same `kernel.bin` behind a stable C ABI,
subsystem by subsystem. The kernel must boot at every commit. NOT a big-bang rewrite, NOT
a greenfield kernel. Reference model = Rust-for-Linux (wrap proven C primitives, don't
reimplement; `unsafe` confined behind safe abstractions; infra-first).

Decisions (2026-06-24): x86_64 only (i686 frozen) → stable Rust + `x86_64-unknown-none`.
"Full pro / no shortcuts" foundation: crate `rust/nanos-kernel` (panic→kernel, GlobalAlloc
with CORRECT alignment over kernel Heap, `SpinLock<T>` WRAPPING the existing C++ ticket
Spinlock incl. its smpPollShootdown spin-poll — not reimplemented). First consumer =
`rust/nanos-mm` replacing FrameAllocator logic (lock moves into Rust; `g_frames` symbol +
call sites unchanged). cbindgen-generated headers (CI-verified), CI = clippy -D + fmt +
miri + cargo test parity vs existing doctest oracle. Already-proven: Rust userland app
exists (`user/rust/rustform` over libnwui via C ABI).

Design committed: docs/superpowers/specs/2026-06-24-rust-kernel-migration-design.md.
Migration order after pioneer: Heap → lib → leaf drivers → fs → net (net last, highest
SMP race value). Aligns with [[no-shortcuts-on-foundations]]; relates to [[nanos-smp-multicore]],
[[nanos-x64-migration]].
