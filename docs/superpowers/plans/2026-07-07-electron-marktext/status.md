# Electron/MarkText Plan Status

Single source of truth for program status (README links here — do not duplicate checkboxes
elsewhere). Update after each gate, not only milestones. Keep entries factual: date, commit, gate,
result. Row format is defined in EXECUTION_GUIDE.md "Work Package Protocol".

## Milestones

- [x] M0 Source Freeze
- [ ] M1 Platform Gap Matrix
- [ ] M2 Node
- [ ] M3 Chromium Content
- [ ] M4 Electron Runtime
- [ ] M5 Generic Packager
- [ ] M6 MarkText Build
- [ ] M7 MarkText Run

## Log

| Date | Commit | Gate | Result |
|---|---|---|---|
| 2026-07-07 | _plan only_ | Plan package created | Pending execution |
| 2026-07-13 | _wip_ | M0 Source Freeze (LOCK-OK/DOC-OK/MATRIX-STUB-OK) | PASS — pinned MarkText 43bd8b7 (0.20.0-dev), Electron 42.1.0, Chromium 148.0.7778.97, Node v24.15.0; electron-stack.lock + electron-platform.md written. Fork locations adapted to NanOS-labs. |
| 2026-07-13 | _wip_ | M1 Task 1.1 Gap Matrix (re-verified vs tree) | PASS — matrix filled. missing-required: eventfd, epoll, pipe2, dup3, mprotect(real RW↔RX, currently no-op stub posixstubs.c:423), /proc/self/exe, /proc/self/fd. Tasks 1.2–1.5 (kernel impl) NOT started; M1 milestone still open. |
| 2026-07-13 | _wip_ | M1 Task 1.2 Event-loop primitives (smoke-eventfd) | PASS — eventfd2 + level-triggered epoll (kernel/Eventfd.h, kernel/Epoll.h, dispatch, libc-glue, sys/{eventfd,epoll}.h). test64 898 cases green; smoke-eventfd (eventfdtest+epolltest, -smp 4) PASS. Fixed a latent x86_64 ABI bug: read/write glue returned int → -1 not sign-extended to ssize_t. Tasks 1.3–1.5 remain. |
| 2026-07-13 | _wip_ | M1 Task 1.3 Real mprotect RW↔RX / V8 W^X (smoke-mprotect) | PASS — was a no-op stub. arch::mmuProtectUser toggles PTE_RW + TLB shootdown; AddressSpace::protect/leafEntry; SYS_mprotect; real userland wrapper. NX not enabled → PROT_EXEC no-op, write-protect via PTE_RW; absent pages skipped (no ENOMEM) to not regress pthread guard pages. test64 899 green (+protect host test); smoke-mprotect (mmapexectest) PASS. No-regression proven: smoke-x86_64 + smoke-smp-stress (8-thread) + smoke-eventfd all PASS isolated. NOTE: a full `make verify64` needs the stray run64-gl QEMU stopped (it holds hostfwd :2222, collides with net smokes). Tasks 1.4–1.5 remain. |
| 2026-07-13 | _wip_ | M1 Task 1.4 /proc/self/{exe,fd} (smoke-procself) | PASS — Process.exe (set at execve, fork-inherited) + ProcInfo.exe; ProcTable::{setExe,openFds,selfPid}; Syscalls::listOpenFds; SynthFs classifyProc `self`-resolution + exe symlink (readlink) + fd directory (readdir). test64 900 green (+setExe/selfPid host test); smoke-procself PASS. FOUND a separate gap (now in matrix): `/tmp` is searchable but NOT user-writable (open O_CREAT as jan → -1) — Node/Electron will need `/tmp` 1777; follow-up. Task 1.5 remains. |

