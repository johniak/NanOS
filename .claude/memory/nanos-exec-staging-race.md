---
name: nanos-exec-staging-race
description: "REAL cause of Dell \"Unknown command nwlogin\" — shared static execve path buffer raced under SMP"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

THE confirmed root cause of the long-running Dell `toybox: Unknown command nwlogin` (tty7 graphical login never came up). It was **NOT** the read path (proven: nwlogin.nxe reads byte-identical to QEMU even under 8-way concurrency), **NOT** the xHCI event-ring race, **NOT** the BOT short-read, and **NOT only** the exec staging window. The real bug:

**`kernel/SyscallDispatch.cpp` SYS_execve marshalled the user path + argv + envp into `static char pathBuf[256]` / `static char strBuf[16384]` / `static argPtrs/envPtrs[]` — SHARED across all CPUs.** At boot init forks 6 gettys (exec `login.nxe` = toybox) beside the tty7 greeter (exec `nwlogin.nxe`). Two concurrent execve on different cores both write the SAME `pathBuf`; the getty's shorter `"/disks/main/nanos/bin/login.nxe\0"` overwrote the greeter's `"/disks/main/nanos/bin/nwlogin.nxe"` at the same basename offset (identical directory prefix) → the greeter exec'd **toybox** with argv[0]="nwlogin" → the error, no tty7. Deterministic on the 4-core Dell; QEMU's single-greeter smoke (smoke-vt) never collided.

**How it was finally found:** a kernel diagnostic (NWDBG) on the nwlogin exec path printed `pp=` (the RESOLVED path) + `st.size` + the staging-window `imageSize`. It showed `pp=/disks/main/nanos/bin/login.nxe stsize=29937 img=5ae0` for a `path` that contained "nwlogin" — i.e. the resolved path itself had become login.nxe. **This reproduced at QEMU -smp 4** (it had not at -smp 1 / single greeter), which is what finally cracked it.

**Fix (branch feat/init-greeter-hardening):** make the four SYS_execve buffers **per-CPU** — `static char pathBuf[arch::SMP_MAX_CPUS][256]` etc., indexed by `arch::smpThisCpu()`. No lock needed: execve runs in ring 0 (not preempted) and its reads busy-poll (never yield), so a task cannot migrate mid-call to another CPU's set. Verified at QEMU -smp 4: NWDBG now shows `pp=...nwlogin.nxe img=8e8` (greeter), no toybox error. All smokes green (x86_64, vt/tty7 desktop, usb, usb-smp, smp4). Build stamp Jun 26 2026 19:48 UTC.

Also kept as valid hardening (same branch): hold `g_execLock` across `archLoadUser` (closes the STAGE_BASE copy-after-unlock window). The earlier "deadlock at 6-way exec-stress" was a TEST bug (forking 6 children hit the 18-task ceiling; waitpid on a failed fork's negative pid hangs) — not a kernel issue. See [[nanos-usb-short-read]], [[nanos-cpu-power-hwp]], [[nanos-meminfo-memtotal]], [[nanos-virtual-terminals]], [[nanos-smp-multicore]].
