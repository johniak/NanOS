# 01 - Chromium Base, IPC, Threads, And Shared Runtime

**Goal:** Build enough of Chromium `base` and IPC support that higher layers fail on display/content
work, not on fundamental OS assumptions.

**Entry criteria:** subplan 00 gate green (`gn gen out/NanOS` succeeds from a clean checkout).

## Decisions

- **Process model: start single-process.** Content shell and first Electron run with
  `--single-process` as the NanOS runtime default until multiprocess is proven. This is a
  supported (if deprecated) upstream mode and removes zygote/sandbox/fd-passing from the critical
  path. Multiprocess (which NanOS `fork`/`exec` + fd passing should eventually support) is a
  documented follow-up gate, not part of M3.
- **Zygote: disabled** (`--no-zygote` + the GN-level no-zygote path); **sandbox: disabled**
  (`--no-sandbox`, per program plan 01 Task 1.8).
- **Crash handling: crashpad/breakpad OFF via supported build flags.** Logging plus NanOS fault
  dumps are the crash story for bring-up; fake crash reporting is forbidden.
- **Shared memory backend: the existing NanOS `memfd_create`** (plan 01 Decisions). If
  Chromium's `PlatformSharedMemoryRegion` insists on seals, that is a gap-matrix row, not a
  Chromium hack.

## Files

Likely patch areas:

- `ports/chromium/patches/` (numbered, standard header)
- Chromium `base/`, `build/`
- NanOS `kernel/Syscall*`, `user/libc-glue/*` (via the program plan 01 protocol only)
- `tests/`, `user/*test.c` (new microtests for newly exposed gaps)

## The Classification Ladder (use for EVERY failure)

Classify each compile/link/runtime failure before fixing it:

1. **Missing libc/POSIX API** → gap-matrix row → microtest → implement in libc-glue/kernel.
2. **Missing syscall** → same protocol.
3. **Chromium platform conditional** → patch with `BUILDFLAG(IS_NANOS)` / `is_nanos` (or
   `is_linux || is_nanos` where the Linux path is genuinely correct).
4. **Unsupported feature** → disable via a supported GN arg in the canonical args file (never
   `#if 0`).
5. **Actual NanOS bug** → fix in NanOS with a test, independent of Chromium.

Work in error clusters: pick the first error, classify, fix, rebuild, commit with the ladder
category in the commit message. Do not chase error counts.

## Steps

- [ ] **Step 1: First build attempt** (expect a wall of errors — that's the work list):

```sh
export PATH="$SDK_WORK/depot_tools:$PATH"
ninja -C "$SDK_WORK/chromium-src/src/out/NanOS" base
```

- [ ] **Step 2: Threading.** `base/threading` must come up on the existing NanOS pthread port:
  create/join, TLS (`thread_local` + pthread keys), condvars/futex waits. Thread names may no-op
  if NanOS lacks them — classify as ladder-3 with a comment. Time: `base/time` needs
  `clock_gettime` MONOTONIC/REALTIME (exists — verify precision assumptions).
- [ ] **Step 3: MessagePump / event primitives.** Chromium's Linux message pump uses epoll
  (plan 01 delivered it). Wire `is_nanos` to the epoll-based pump. If a probe wants `timerfd` or
  `eventfd` semantics beyond what exists, follow the missing-API protocol before patching
  Chromium.
- [ ] **Step 4: Shared memory.** Point `base/memory/platform_shared_memory_region*` at the
  memfd path for `is_nanos`. Prove with `shmdualtest` semantics; if seals are demanded, record
  and implement seals (gap matrix first).
- [ ] **Step 5: Process launching.** For single-process mode this is mostly unused, but
  `base/process` must compile: map to fork/exec/waitpid, stub out priorities/OOM-score with
  ladder-3 conditionals.
- [ ] **Step 6: Crash/debug.** Disable crashpad/breakpad via GN (`use_crashpad`-family args for
  the milestone); `base/debug/stack_trace` may degrade to a no-op printer with a comment.
- [ ] **Step 7: Optional confidence target** once `base` links:

```sh
ninja -C "$SDK_WORK/chromium-src/src/out/NanOS" base_unittests
```

  Building it is valuable even if running the full suite on NanOS is deferred; if it links, run a
  filtered subset in QEMU (`--gtest_filter=*Time*:*Lock*`) and record results.

## Required Microtests

Program plan 01 already delivered `eventfdtest`, `epolltest`, `mmapexectest`, `procselftest`,
`shmdualtest`. Add new `user/*test.c` microtests for any additional gap this subplan exposes
(registration recipe in EXECUTION_GUIDE) — a Chromium-observed bug must reproduce in a microtest
before the kernel/libc fix lands.

## Gate

```sh
ninja -C "$SDK_WORK/chromium-src/src/out/NanOS" base && echo BASE-OK
make test64
```

- [ ] `BASE-OK` prints — or the build stops at a documented blocker **outside** `base/`
  (record the exact target + first error in `status.md` and `ports/chromium/README.md`).
- [ ] All QEMU microtests still PASS; `make test64` green.
- [ ] Gap matrix has no untracked base/IPC assumptions (every ladder-1/2 fix has a row).
- [ ] `status.md` row added; parent coordinator checkbox 01 ticked.
