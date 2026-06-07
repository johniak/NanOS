# NanOS — Multiprocessing Roadmap & Status

Goal: Linux-style multiprocessing with Unix `fork()` and a `task` abstraction.
True `fork()` needs per-process address spaces → paging → ring 3 → scheduler → fork.

## Stages

| Stage | What | Status |
|------|------|--------|
| **1** | **Paging foundation** — physical frame allocator + enable `CR0.PG`, kernel identity-mapped, reusable `AddressSpace` (page dir + map/unmap/translate + CR3 switch). Ring 0, single program. | ✅ DONE |
| **(arch refactor)** | **Linux-style MI/MD split** — all x86 code under `arch/x86/{boot,cpu,mm,drivers}` behind `<arch/...>` contracts (console, bootinfo, mmu, irq, cpu, syscall, block, usermode); build selects `ARCH` via `arch/x86/arch.mk`; `make check-arch` guards the boundary. Prepares for an ARM/RPi port (= add `arch/arm/` implementing the contracts). | ✅ DONE |
| **2** | **init in ring 3 + own address space** — single process, no scheduler. init runs at CPL 3 in a private page directory (kernel half shared supervisor, user window `[0x400000,0x500000)` = fresh private frames), syscalls via `int 0x80` (TSS `esp0`), exits back to the kernel, runs twice; provably isolated (user touching kernel mem → `#PF`, no triple fault). | ✅ DONE |
| **3** | **Scheduler + `task` abstraction** — ✅ preemptive round-robin on the 1000 Hz PIT (IRQ0). MI `Scheduler` (task table, states READY/RUNNING/BLOCKED/DONE, round-robin, idle task PID 0); arch `archContextSwitch` (`switch.S`: callee-saved + esp + live CR3) + `archTaskBootstrap` (`ret_from_fork`-style). Boots into idle + init/nsh (the one ring-3 task) + a kernel clock thread. Blocking `read` yields (block/wake on keyboard IRQ) — no more hlt-spin. `/proc/uptime` is real (tick counter). Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-stage3-scheduler*`. | ✅ DONE |
| **4** | **`fork` / `exec` / `wait` / `exit`** — Unix process model. fork = **eager** address-space copy (decided), exec reuses the loader into the current space, wait/exit with zombies. | ⬜ TODO |
| **(synthetic root FS)** | **`/` is a synthetic in-memory filesystem** (`SynthFs`), not a physical volume — deliberately non-Unix. It holds `/disks` (mounted volumes), `/dev` (`null`/`zero`/`random`), `/proc` (`uptime`); the system disk mounts at **`/disks/main`** (programs at `/disks/main/bin`). Node kinds: dir / static / generated. Plugs into the existing prefix-routing `Vfs` (+ a `mount(mp, FileSystem*)` overload). Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-synthetic-root-fs*`. | ✅ DONE |
| **(shell + coreutils)** | **`nsh` shell + verbatim sbase `cat`/`ls` on a ported picolibc.** Done as a side-project on top of stage 2: a synchronous `SYS_spawn` (nested ring-3 exec, second kernel stack), argv on the user stack, cooked blocking stdin (keyboard line discipline), `SYS_stat` + ext metadata so `ls -l` shows real mode/size/owner. Userland gained a real libc (picolibc, built in Docker) + a thin syscall/sbrk/dirent/cwd/pwd-grp glue layer; `cat.c`/`ls.c`/libutf are unmodified upstream. Boots into `nsh$`. nsh runs a raw-mode readline (bash-like): in-RAM history (↑↓), cursor editing (←→, mid-line insert/backspace), via a cooked/raw console mode (`SYS_termmode`) + a host-tested `KeyDecoder`. | ✅ DONE |
| **later** | **`.ndl` (Nano Dynamic Library)** — full Windows-style model: the libc becomes a separate `.ndl` the kernel loads into the process (ring 3), program's IAT bound to it; the `.ndl` issues `int 0x80`. (Today picolibc + glue are statically linked into each program and trap directly — no IAT.) Also: `.nkext` kernel modules; ext write support; a getdents64 read cursor (today it re-lists the whole dir per call, so readdir reads once, capping a single listing at the buffer). | ⬜ TODO |

## Key design decisions (so they survive compaction)

- **Memory model:** real paging, per-process page directories (chosen over flat/clone-style).
- **Ring 3 now**, scheduler later (split the old "stage 2" into ring-3-single-process + scheduler).
- **fork = eager copy** first (COW later).
- **Syscall ABI:** Linux i386 numbers (`kernel/SyscallNr.h`, shared C header), `int 0x80`. Our numbers are stable (we own them).
- **Executable model = Windows-ish:** stable named API is the contract; syscall numbers hidden behind libnanos (the ntdll/glibc analog). `.nxe` program, `.ndl` lib, `.nkext` kext naming.
- **Per-process dir construction:** `AddressSpace::adoptKernelDirectory` — memcpy all 1024 kernel PDEs (kernel half shared, supervisor), clear the user-window PDE so user maps get a fresh private PT. `mmuDestroyAddressSpace` currently LEAKS (copied PDEs alias kernel PTs; selective free is a TODO).
- **Exit path:** `int 0x80 SYS_exit` → `Syscalls::exit()` sets a flag → syscall trap checks `hasExited()` → `arch::userExit()` (switch CR3 to kernel, `nx_longjmp` to `arch::enterUser`). `Syscalls::resetForRun()` clears the flag between runs. longjmp val is fixed `1`; real code via `Syscalls::code()`.
- **TSS:** dedicated 8 KiB kernel stack `g_userKstack` for `esp0` (separate from the boot stack). GDT = 6 entries (null, ring0 code 0x08/data 0x10, ring3 code 0x1B/data 0x23, TSS 0x28).

## Where things live (post arch-split)
- Contracts: `arch/include/arch/{console,bootinfo,mmu,irq,cpu,syscall,block,usermode}.h`
- x86 ring-3/exec: `arch/x86/cpu/usermode_x86.cpp` (enterUser/userExit/execUserImage, g_userCtx, g_userKstack), `arch/x86/cpu/Gdt.*` + `Tss.h` (TSS), `arch/x86/cpu/fault_x86.cpp` (#PF/#GP), `arch/x86/cpu/syscall_x86.cpp` (int 0x80 decode + hasExited hook).
- MMU: `arch/x86/mm/mmu_x86.cpp` (per-process API), `arch/x86/mm/AddressSpace.*` (host-tested).
- MI: `kernel/Exec.cpp` (stage image → `arch::execUserImage`), `kernel/SyscallDispatch.cpp` (`kernelSyscall` switch), `kernel/Syscall.*` (Syscalls core), `mm/FrameAllocator.*`.
- Userland: `user/{crt0.S,libnanos.c/h (int 0x80),nxhdr.c (importCount=0),init.c,nx.ld}`.

## Verify (per CLAUDE.md)
`make build` (Docker) · `make test` (doctest, ≥90% lcov, currently 83 tests ~95.9%) ·
`make check-arch` (MI boundary) · QEMU headless screendump + `-d int` fault grep
(ring-3 proof = `int 0x80` traps at `cpl=3`; success = grub.cfg + "init.nxe exited with code 0" ×2, no `v=08/0d/0e`).

## Loose ends
- `Console::writeHex` (drivers/Console.cpp): single-hex-digit padding doesn't re-terminate the
  string → trailing garbage (visible in the fault handler). Pre-existing; 1-line fix.
- `user/init.c` has an uncommitted debug line `write(1,"test\n",5)` (predates stage 2).
- Branch `dockerized-build` is many commits ahead of `origin/master`, not pushed.

_Last updated: Stage 3 landed (preemptive scheduler + task abstraction, 1000 Hz PIT,
block/wake, real /proc/uptime). Next: Stage 4 (fork/exec/wait — needs per-task
g_userCtx/esp0 once there is >1 ring-3 task), then the `.ndl` libc._
