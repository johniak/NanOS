---
name: nanos-git-port
description: GNU git 2.54 ported via the i686-nanos SDK — builds/runs/commits on NanOS; git gc still TO-FIX
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

GNU **git 2.54.0** is ported to NanOS via the i686-nanos SDK as the "real `-lpthread` + `fork()` app" gate for the pthread work ([[nanos-pthread-port]]). **It builds, loads, runs, and `git commit`s on the ext4 disk** (real objects/refs written, 0 faults) — git is pthread-linked and fork+execs its subcommands. This is a sufficient demonstration that a real fork+pthread app runs on NanOS.

Build source lives OUTSIDE this repo: `~/Projects/nanos-sdk-work/git-port/git-2.54.0/` (a copy of upstream + `config.mak` + 2 small `run-command.c` patches). `~/Projects/git` is a *separate* clone, NOT the build tree. Build with `make git` (+ `make image`). Full status + reproduction in `docs/superpowers/plans/2026-06-16-git-port-followup.md` (committed).

Run git from `cd <repo>` form (NOT `git -C` — see below). `git init/config/add/commit/log` work.

**STILL TO FIX (git is "do poprawy"):**
- **`git gc`** fails in the repack↔pack-objects pipe: `fatal: repack: Expecting full hex object ID lines only from pack-objects`. pack-objects (threaded) runs, but repack can't parse its piped stdout. Last blocker to the full gc thread demo. Look at git `builtin/repack.c` + NanOS pipe/dup2 in `start_command`.
- **`git -C <path>`** resolves its lockfile against the read-only `/` (SynthFs root) → EROFS; our `chdir`/`getcwd` are correct (verified), it's a git startup-cwd quirk. Workaround: `cd <repo>`.

General fixes that landed enabling this (commit `9b356f2`, branch `feat/pthread`) — all reusable, not git-only: libc `getdelim`/`getline` return -1 at EOF (POSIX; picolibc returned 0); libc `execve` retries with `.nxe` on ENOENT (NanOS exec convention for bare-name execs); `make git` refreshes crt0.o/nxhdr.o into the SDK sysroot (main-thread TLS bootstrap — fixes all SDK ports post per-thread-errno migration); git-core installed as `.nxe` hardlinks.
