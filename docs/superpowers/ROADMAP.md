# NanOS — Multiprocessing Roadmap & Status

Goal: Linux-style multiprocessing with Unix `fork()` and a `task` abstraction.
True `fork()` needs per-process address spaces → paging → ring 3 → scheduler → fork.

## Next up (priority order)

1. **Basic coreutils — the essential file/dir toolkit. ✅ DONE.** The shell can now create, remove,
   copy, move, and link files (closing the `bash: touch: command not found` gap from the git port).
   Shipped as in-tree **sbase** ports (verbatim upstream @ commit `c546c3a`, built like `cat`/`ls`,
   dynamically linked against `libc.ndl`, installed to `/nanos/bin`). Plan + per-task log:
   `docs/superpowers/plans/2026-06-16-coreutils.md`.

   **Tier 1 (shipped):** `mkdir`, `rmdir`, `rm` (`-r`/`-f`), `touch`, `mv`, `cp` (`-r`), `ln`
   (`-s`), `pwd`. **Tier 2 (shipped):** `chmod`, `wc`, `head`, `tail`, `true`, `false`, `env`,
   `basename`, `dirname`. (`stat` was dropped — sbase has none and `ls -l` already shows
   mode/size/owner/mtime.)

   Latent kernel/libc bugs found + fixed along the way (all surfaced by exercising the tools):
   `chmod`/`chown`/`fchmod`/`fchown` were no-op stubs (now real syscalls); the whole `*at` family +
   `utimes`/`utimensat`/`futimens` libc wrappers were missing; the `*at` constants mismatched
   (picolibc newlib values vs the kernel's Linux ABI — kernel now accepts both); `stat` reported
   `st_ino=1` for every file (now real inodes, also fixing hard-link identity); `mkdir -p` aborted
   on read-only parents (EEXIST-before-EROFS ordering in SynthFs/Vfs); libc `rename` did a
   copy+unlink stopgap (now real `SYS_rename`, so `mv` moves directories).

   Limitations (by design): `pwd -P`/`stat` rely on `ls -l`/`getcwd` (no `realpath`); `cp -a` of
   device/fifo nodes is unsupported (no `mknod` syscall); `touch -d …Z` treats Zulu as UTC
   (NanOS is UTC-only, no `tm_gmtoff`).

## Stages

| Stage | What | Status |
|------|------|--------|
| **1** | **Paging foundation** — physical frame allocator + enable `CR0.PG`, kernel identity-mapped, reusable `AddressSpace` (page dir + map/unmap/translate + CR3 switch). Ring 0, single program. | ✅ DONE |
| **(arch refactor)** | **Linux-style MI/MD split** — all x86 code under `arch/x86/{boot,cpu,mm,drivers}` behind `<arch/...>` contracts (console, bootinfo, mmu, irq, cpu, syscall, block, usermode); build selects `ARCH` via `arch/x86/arch.mk`; `make check-arch` guards the boundary. Prepares for an ARM/RPi port (= add `arch/arm/` implementing the contracts). | ✅ DONE |
| **2** | **init in ring 3 + own address space** — single process, no scheduler. init runs at CPL 3 in a private page directory (kernel half shared supervisor, user window `[0x400000,0x500000)` = fresh private frames), syscalls via `int 0x80` (TSS `esp0`), exits back to the kernel, runs twice; provably isolated (user touching kernel mem → `#PF`, no triple fault). | ✅ DONE |
| **3** | **Scheduler + `task` abstraction** — ✅ preemptive round-robin on the 1000 Hz PIT (IRQ0). MI `Scheduler` (task table, states READY/RUNNING/BLOCKED/DONE, round-robin, idle task PID 0); arch `archContextSwitch` (`switch.S`: callee-saved + esp + live CR3) + `archTaskBootstrap` (`ret_from_fork`-style). Boots into idle + init/nsh (the one ring-3 task) + a kernel clock thread. Blocking `read` yields (block/wake on keyboard IRQ) — no more hlt-spin. `/proc/uptime` is real (tick counter). Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-stage3-scheduler*`. | ✅ DONE |
| **4** | **`fork` / `exec` / `wait` / `exit`** — ✅ Unix process model on the **trap-frame + scheduler** model (a process is a scheduler task that runs until `exit()`; syscalls `iret`-return; blocking via `Scheduler::block/wake`). `fork` = **eager** copy (`mmuCopyAddressSpace`: new dir sharing the kernel half + user window copied frame-by-frame; fd table dup'd) + `archForkChild` (child kernel stack fabricated → `ret_from_fork` `iret`s a copy of the parent trap frame with `eax=0`). `execve` loads a `.nxe` into a fresh space and **rewrites the live trap frame** in place. `waitpid` blocks then reaps the zombie (frees space + task slot + fd table). `exit` frees the address space (`mmuFreeAddressSpace`, selective — no kernel-PT leak), zombifies, wakes the parent. Per-process `esp0` (scheduler sets `TSS.esp0` on each switch); task slots recycled (`TASK_FREE`). PID 1 (init) `execve`s nsh; nsh runs commands via fork/exec/wait. `SYS_spawn` removed. Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-stage4-fork-exec-wait*`. | ✅ DONE |
| **5** | **Signals + job control** — ✅ a real Unix signal subsystem. MI core `kernel/Signal.*` (per-process pending/blocked masks + disposition table; `sigPost`/`sigNextDeliverable`/`sigResolve`/`sigDefaultAction`/`sigHasInterrupt`/fork-inherit/exec-reset; host-tested). Syscalls `kill(37)`/`signal(48)`/`sigreturn(119)`/`sigprocmask(126)`. Delivery at every **return to ring 3** (syscall + IRQ return, gated on `(cs&3)==3`): default action terminates (`WIFSIGNALED`) or stops; **catchable handlers** run in ring 3 via a user-stack signal frame (`archPushSignalFrame`) + a libc trampoline (`user/sigtramp.S` → `SYS_sigreturn` restores the frame + the pre-handler mask, eflags sanitized). Blocking `read()` is interruptible and **restarts** (`SA_RESTART`, on by default like glibc `signal()`): the syscall returns an internal `-ERESTARTSYS`, and delivery either rewinds `eip` to re-issue `int 0x80` (restart) or rewrites it to `-EINTR`, per a per-signal restart flag. Interruptibility is gated on `sigHasInterrupt` so ignored signals (SIGCHLD) don't spuriously interrupt a `wait`. Tty: cooked-mode Ctrl+C→SIGINT, Ctrl+\→SIGQUIT, Ctrl+Z→SIGTSTP to the foreground process (`consoleSignal`); `KeyDecoder` tracks the Ctrl modifier. **Job control:** `TASK_STOPPED` + `procStop` (returns on SIGCONT), `signalSend` resumes a stopped task on SIGCONT, `waitpid` `WNOHANG`/`WUNTRACED` + stopped reporting (`ProcTable::reapStopped`), nsh job table + `jobs`/`fg`/`bg` + background reaping. Spec: `docs/superpowers/specs/2026-06-08-signals-job-control-design.md`. | ✅ DONE |
| **6** | **Graphics — firmware framebuffer (vesafb/fbcon) + Linux `/dev/fb0`** — ✅ done as Linux does with GRUB. The Multiboot header requests a graphics mode (`loader.s` VIDEO flag + 1024×768×32); GRUB sets it (VBE/GOP) and reports a linear framebuffer in the Multiboot info (`MultibootInfo` fb fields + `multibootFramebuffer()`), exposed via `arch::bootFramebuffer()`. `mmuMapKernelMmio` identity-maps the LFB (it lives above RAM) into the kernel before any process space exists. MI software renderer `drivers/Framebuffer.*` (put-pixel/fill-rect/blit-glyph/scroll, stride by pitch, 32+24bpp) + public-domain IBM VGA `drivers/Font8x16.*`. `drivers/FbConsole.*` is a text console over the framebuffer (Linux fbcon); `console_x86` selects fbcon vs VGA-text at boot (`consoleActivateFramebuffer`). **`/dev/fb0`** Linux-compatibly: `drivers/Fbdev.*` (real `fb_fix/var_screeninfo` + `FBIOGET_*SCREENINFO`), `CharDevice`/`Fb0Device`, `SynthFs SK_CHARDEV`, `Vfs`/`Syscalls` `write/ioctl/mmap` (`SYS_ioctl 54`, `SYS_mmap2 192`), `mmuMapUserFb` (FB at user VA `0x10000000`, mapped under the kernel dir). `user/fbtest.c` draws a gradient through mmap. Spec: `docs/superpowers/specs/2026-06-08-...` (the framebuffer plan). | ✅ DONE |
| **(synthetic root FS)** | **`/` is a synthetic in-memory filesystem** (`SynthFs`), not a physical volume — deliberately non-Unix. It holds `/disks` (mounted volumes), `/dev` (`null`/`zero`/`random`), `/proc` (`uptime`); the system disk mounts at **`/disks/main`** (programs at `/disks/main/bin`). Node kinds: dir / static / generated. Plugs into the existing prefix-routing `Vfs` (+ a `mount(mp, FileSystem*)` overload). Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-synthetic-root-fs*`. | ✅ DONE |
| **(shell + coreutils)** | **`nsh` shell + verbatim sbase `cat`/`ls` on a ported picolibc.** Done as a side-project on top of stage 2: a synchronous `SYS_spawn` (nested ring-3 exec, second kernel stack), argv on the user stack, cooked blocking stdin (keyboard line discipline), `SYS_stat` + ext metadata so `ls -l` shows real mode/size/owner. Userland gained a real libc (picolibc, built in Docker) + a thin syscall/sbrk/dirent/cwd/pwd-grp glue layer; `cat.c`/`ls.c`/libutf are unmodified upstream. Boots into `nsh$`. nsh runs a raw-mode readline (bash-like): in-RAM history (↑↓), cursor editing (←→, mid-line insert/backspace), via a cooked/raw console mode (`SYS_termmode`) + a host-tested `KeyDecoder`. | ✅ DONE |
| **later** | **`.ndl` (Nano Dynamic Library)** — full Windows-style model: the libc becomes a separate `.ndl` the kernel loads into the process (ring 3), program's IAT bound to it; the `.ndl` issues `int 0x80`. (Today picolibc + glue are statically linked into each program and trap directly — no IAT.) Also: `.nkext` kernel modules; ext write support; a getdents64 read cursor (today it re-lists the whole dir per call, so readdir reads once, capping a single listing at the buffer). | ⬜ TODO |

## Key design decisions (so they survive compaction)

- **Memory model:** real paging, per-process page directories (chosen over flat/clone-style).
- **Ring 3 now**, scheduler later (split the old "stage 2" into ring-3-single-process + scheduler).
- **fork = eager copy** first (COW later).
- **Syscall ABI:** Linux i386 numbers (`kernel/SyscallNr.h`, shared C header), `int 0x80`. Our numbers are stable (we own them).
- **Executable model = Windows-ish:** stable named API is the contract; syscall numbers hidden behind libnanos (the ntdll/glibc analog). `.nxe` program, `.ndl` lib, `.nkext` kext naming.
- **Per-process dir construction:** `AddressSpace::adoptKernelDirectory` — memcpy all 1024 kernel PDEs (kernel half shared, supervisor), clear the user-window PDE so user maps get a fresh private PT. Teardown is now **selective** (`mmuFreeAddressSpace`/`AddressSpace::freeUserWindow`): free the user-window frames + the private user PT + the directory, never the shared kernel PTs. `mmuCopyAddressSpace`/`copyUserWindowFrom` does the fork copy.
- **Exit/enter path (Stage 4 trap-frame model):** `archEnterUser` switches CR3 and `iret`s to ring 3 (never returns); a syscall returns by `iret`-ing the trap frame (`Registers`) on the process kstack. `SYS_exit` sets a per-process flag → the syscall trap calls `kernel::procExit()` (free space, `TASK_ZOMBIE`, wake parent, `schedule()` away). `execve` rewrites the live frame (`archFrameToUser`). `fork` fabricates the child stack → `ret_from_fork` (the ISR return tail in `isr.S`). The old `g_userCtx`/`nx_longjmp`/`spawnUserImage` are gone.
- **TSS / esp0:** per-process kernel stack — the scheduler repoints `TSS.esp0` at the running task on every switch (`arch::setKernelStack`); `cpuInit` uses a single boot kstack until then. GDT = 6 entries (null, ring0 code 0x08/data 0x10, ring3 code 0x1B/data 0x23, TSS 0x28).

## Where things live (post arch-split)
- Contracts: `arch/include/arch/{console,bootinfo,mmu,irq,cpu,syscall,block,usermode}.h`
- x86 ring-3/exec: `arch/x86/cpu/usermode_x86.cpp` (`archLoadUser`/`archEnterUser`/`archFrameToUser`), `arch/x86/cpu/fork_x86.cpp` (`archForkChild`), `arch/x86/cpu/isr.S` (`ret_from_fork`), `arch/x86/cpu/Gdt.*` + `Tss.h` (TSS), `arch/x86/cpu/fault_x86.cpp` (#PF/#GP), `arch/x86/cpu/syscall_x86.cpp` (int 0x80 decode + procExit hook), `arch/x86/cpu/sched_x86.cpp` + `switch.S` (context switch / PIT).
- MMU: `arch/x86/mm/mmu_x86.cpp` (per-process API: create/copy/free/switch), `arch/x86/mm/AddressSpace.*` (host-tested; adopt/free/copy user window).
- MI: `kernel/Process.*` (process table + fork/wait reap bookkeeping), `kernel/Exec.cpp` (execProgram/execve/fork/wait/procExit), `kernel/Scheduler.*` (tasks, slots, block/wake/reap), `kernel/SyscallDispatch.cpp` (`kernelSyscall` switch), `kernel/Syscall.*` (per-process Syscalls core), `mm/FrameAllocator.*`.
- Userland: `user/{crt0.S, init.c (execve's nsh), nsh.c (fork/exec/wait), nx.ld}` + `user/libc-glue/` (picolibc port: syscalls incl. fork/execve/waitpid, sbrk, dirent, cwd).

## Verify (per CLAUDE.md)
`make build` (Docker) · `make test` (doctest, ≥90% lcov, currently 83 tests ~95.9%) ·
`make check-arch` (MI boundary) · QEMU headless screendump + `-d int` fault grep
(ring-3 proof = `int 0x80` traps at `cpl=3`; success = grub.cfg + "init.nxe exited with code 0" ×2, no `v=08/0d/0e`).

## Loose ends
- `Console::writeHex` (drivers/Console.cpp): single-hex-digit padding doesn't re-terminate the
  string → trailing garbage (visible in the fault handler). Pre-existing; 1-line fix.
- `user/init.c` has an uncommitted debug line `write(1,"test\n",5)` (predates stage 2).
- Branch `dockerized-build` is many commits ahead of `origin/master`, not pushed.

_Last updated: Stage 4 landed (fork/exec/wait/exit — trap-frame + scheduler process
model, eager fork, selective address-space teardown, per-process esp0, zombie reap;
nsh runs commands via fork/exec/wait; SYS_spawn removed). The multiprocessing roadmap
(stages 1–4) is complete. Next: the `.ndl` libc, `.nkext` modules, ext write support._
