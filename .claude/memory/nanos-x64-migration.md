---
name: nanos-x64-migration
description: x86_64 migration is a REPLACEMENT (no pre-x64 CPU support) not a dual-arch add; analysis doc location
metadata: 
  node_type: memory
  type: project
  originSessionId: d902bbc1-34bd-4054-bb9b-e8970696fc59
---

NanOS migration i686 → x86_64 (long mode). **Key constraint: we do NOT need to support
old CPUs without x86_64.** Therefore the port is a **replacement**, not adding a parallel
`arch/x86_64` kept green alongside `arch/x86`.

**Why:** removes all dual-arch maintenance — no 32/64 CI matrix, no back-compat. Lets us
break shared MI signatures freely (e.g. `kernelSyscall` args → `uintptr_t`, `arch/*`
contracts → 64-bit), commit to LP64, drop `i686-elf`/`i686-nanos` toolchains entirely
(single `x86_64-*`), ship only `.nxe` format v4 (no v3 loader), and assume x86_64 baseline
features (SSE2, NX, `syscall` instruction always present).

**How to apply:** create `arch/x86_64/` as the only supported arch impl, keep `arch/x86`
only as reference during port, then DELETE it after first green x86_64 boot. Keep the
`ARCH` switch + MI/MD split (cheap, useful for future ARM). Rebuild all ~12 ported apps
once for `x86_64-nanos` (one-time, not maintained in parallel).

Full analysis: `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md` (decision
table, MD rewrite inventory, LP64 MI hazards in §4, 8 milestones in §7). Per-phase executable
plans: `docs/superpowers/plans/2026-06-15-x86_64-plan-{1..8}-*.md`.

**Progress (on branch `feat/x86_64-foundation`, local/unpushed):** Plans 1-5 + 7 DONE and
verified. Plan 1 (toolchain + long-mode boot), 2 (MI kmain + console), 3 (4-level PML4 paging
+ NX, QEMU-verified), 4 (GDT/IDT/TSS + interrupts — PIT+keyboard fire, 0 faults), 5 (ATA64 +
ext read/WRITE on x86_64 — boots from a GRUB disk image, mounts /disks/main, JBD2 write
persists across reboot, e2fsck-clean), 7 (MI LP64 sweep, merged). The x86_64 kernel is still
**STAGED** (`arch/x86_64/boot/KernelStage64.cpp` provides its own minimal `Kernel::start`,
linked from `STAGE64_OBJS`) — a full `make ARCH=x86_64 _all` link of the real `kernel/Kernel.cpp`
is NOT yet possible (needs x86_64 syscalls/scheduler from Plan 6+). Build/run x86_64: `make image64`
/ `make run64` (boots from disk); `make bringup64` (rescue ISO). i686 stays green throughout
(arch/x86 not yet deleted). REMAINING: Plan 6 (syscall/sysret + userland + .nxe v4 — biggest,
un-stages the kernel), Plan 8 (SDK x86_64-nanos + app ports), Plan 7 T12-14 (deferred contract
widening + flip the -Wconversion gate). Carry-forward paging gates: [[nanos-x64-paging-carryforward]].
A real bug found+fixed in Plan 5: x86_64 boot stack was 16 KiB (loader.S), too small for the
ext block-buffer recursion under LP64 — grown to 1 MiB (i686 already had ~1.5 MiB).

**Plan 6 DONE (ring-3 reached):** the first 64-bit userland program runs in ring 3 — `init.nxe`
(SysV crt0, `syscall` instruction) cats grub.cfg from ring 3 and `exit()`s back to the kernel,
zero faults. Delivered: x86_64 syscall numbers, `.nxe` v4 (NxeLoader + mknx64, R_X86_64_64),
nxjmp64, syscall_entry64.S (swapgs/sysret), syscall MSR init (EFER/STAR/LSTAR/FMASK),
usermode_x86_64 (iretq to ring3 + %fs.base), userland build (SSE on for user / off for kernel).
**GDT-SYSRET fix:** Plan 4's GDT was SYSRET-incompatible; reordered to user32-placeholder@0x18 /
udata@0x20 / ucode64@0x28 / TSS@0x30 (ring3 CS=0x2B/SS=0x23), interrupt boot re-verified.

**Plan 9 (UN-STAGE) DONE:** the real `kernel/Kernel.cpp` now links via `make ARCH=x86_64 _all`
and boots from disk — full init path (paging → ext mount `/disks/main` → JBD2 write self-test
persists across reboot → tmpfs → /dev/input0+PTY → PCI enum (e1000) → kext load → scheduler
start, PIT ticking) with **`init.nxe` running as PID 1 in ring 3** (writes banner + cats grub.cfg
via syscalls), ZERO faults, e2fsck-clean, i686 green (613) / x86_64 host tests green (620).
Delivered the last MD ports: scheduler context switch (`switch64.S`+`sched_x86_64`), fork/clone
(`fork_x86_64`), CSPRNG (`random_x86_64` RDRAND/RDTSC), PCI (`arch/x86_64/io/pci_x86_64`),
keyboard line-discipline (`input_x86_64`), and widened `<arch/sched.h>` to LP64. Key build fix:
x86_64 kernel objects build into a SEPARATE dir `bin/k64/` (per-arch KOBJ) so ELF32 i686 and
ELF64 x86_64 objects never collide in `bin/`. `syscallSelfTest` is a no-op on x86_64 (ring0→ring0
SYSRET can't round-trip — init.nxe is the real proof). The staged `KernelStage64.cpp` remains
ONLY for the `bringup64` rescue-ISO smoke. Run/build x86_64: `make image64` / `make run64`.
Plan doc: `docs/superpowers/plans/2026-06-18-x86_64-plan-9-unstage.md`.

**Plan 8 8b DONE (toolchain):** the `x86_64-nanos` cross-toolchain is built in `~/Projects/nanos-sdk`
(commits f1477f5/279ce1c/e41c873/a271956): binutils 2.43 + gcc 14.2.0 (`-dumpmachine x86_64-nanos`,
ELF64, `__LP64__`/`__SSE2__`, LINK_SPEC `-T nx.ld --emit-relocs ...` = decision #1) + picolibc 1.8.6
x86_64 (`libc.a`). patch.sh corrections vs the plan's stale strings: gas needs no x86_64 rule
(maps to i386), gcc tm_file uses `i386/x86-64.h` (not `x86-64elf.h`), libgcc anchor is the
combined `| rtems` line.

**Plan 8 8b Task 6 (hello.nxe smoke) BLOCKED → reveals the real 8a gap:** Plan 6's "userland done"
was only the MINIMAL freestanding `init.nxe` (`user/libnanos.c`, direct `syscall`) — the FULL
picolibc/musl `libc.ndl` (which ALL Plan 8 apps need) is NOT built for x86_64. Two blockers:
(1) the userland **musl pthread/TLS arch layer is i386-only** — `user/libc-glue/pthread/include/
{atomic_arch.h (uses %esp), pthread_arch.h (movl %gs:0)}`, `syscall_arch.h`, `nx-tcb.h` TCB field
offsets (4 vs 8 B), and the crt0 TLS bootstrap (`%gs`/`set_thread_area` → `%fs`/`arch_prctl`) all
need an x86_64 port; (2) the NanOS `Makefile` `libc.ndl`/import-lib rules (~lines 1176-1196) are
hard-wired 32-bit (`$(MKNX)` not `$(MKNX64)`, `nasm -f elf` not `-f elf64`) and the `ifeq
($(ARCH),x86_64)` userland block only builds the minimal init. **RESUME HERE:** port the userland
musl pthread/TLS arch layer to x86_64 (%fs.base TLS via arch_prctl — the kernel side is done in
Plan 6, this is the userland TCB/pthread side; see [[nanos-pthread-port]] for the i686 port) +
make the libc.ndl Makefile rules arch-aware, THEN the 4-artifact contract is truly 64-bit and the
hello.nxe smoke + the ~12 app ports proceed.

**Plan 10 10a + 10b DONE — the full 64-bit libc works in ring 3.** Plan doc:
`docs/superpowers/plans/2026-06-18-x86_64-plan-10-userland-libc-and-ports.md`. Built the real
`libc.ndl`/`libc.ndl.a`/`crt0.o` (ELF64, v4): ported the userland musl arch layer (atomic_arch
64-bit, pthread_arch `%fs:0` TP, syscall_arch `syscall`-insn, 64-bit TCB), crt0 TLS bootstrap via
`arch_prctl(ARCH_SET_FS)`, wired the kernel `SYS_arch_prctl` dispatch + made `archLoadThreadTls`
actually set `%fs.base` (was a no-op stub), arch-aware `libc.ndl` Makefile rules, and added an
x86_64-elf picolibc layer to `docker/Dockerfile` (non-PIC/small, `/opt/picolibc/x86_64-elf`).
**Runtime-proven in QEMU ring 3, zero faults:** a picolibc `hello.nxe` prints, and a pthread smoke
shows `errno@<addr>` (%fs.base TLS works) + `thread running` + `joined ok` (pthread_create/join).
The runtime gate found+fixed 6 real bugs: mknx import thunks were i386 (→ RIP-relative
`jmp [rel slot]`), the `.nxe` loader dropped `R_X86_64_32S` base relocs (errno/TCB kept link-time
base → 4-byte loader patch `NX_RELOC_W32`), libc-glue used `int 0x80` (→ SYSCALL), **the kernel had
SSE off so picolibc `movups` #UD'd — `cpuInit` now enables CR4.OSFXSR/OSXMMEXCPT + CR0.MP/clear EM**,
clone TLS ABI was i386 (x86_64 swaps clone args, tls = raw %fs.base), and the freestanding init.nxe
link. i686 byte-for-byte unchanged (SHA-256 verified), 613 host tests 91%. Caveats: libc syscall
trampolines still take `int` args (user ptrs must stay <2 GiB — true for the current map; full LP64
wrapper widening is a follow-up); shared `bin/` glue objects aren't `64`-suffixed so `make clean`
between arch switches is required.

**Plan 10 10c PROGRESS (app ports on x86_64):** DONE + QEMU-verified (zero faults, exec-path
since no PS/2 keyboard kext yet): **nsh + 22 coreutils** (commit `ff2e6a0`; fixes: crt0 publishes
environ `73bb6c5`, userland `knl_stat` matched to the 48-byte kernel LinuxStat `68ad90a`),
**bash 5.2** (`bash-5.2#` prompt as login shell; NanOS `bdccff1`, fork `~/Projects/bash-nanos`
branch `x86_64-port` `ddcedcb`; LP64 fix = config.cache sizeof_long/ptr=8), **grep + bzip2**
(NanOS `7d12739` + `scripts/nx-gcc`/`nx-port-build.sh`/`posix-hosted-patch.sh`). x86_64-specific
gotcha solved: RIP-relative refs to `stdin/stdout/stderr/environ` can't MinGW-auto-import like
i686 `R_386_32` — use the `nx-dllimport.h` shim (force-include or appended to config.h).

**TWO bigger gaps discovered for "100% apps/libs on x64":**
1. **Ecosystem libraries are i686-ONLY** — the x86_64-nanos sysroot has only libc/libm. Every
   `lib*.a` (ncurses/libtinfo, zlib, libpng, libjpeg, openssl's deps, and NetSurf's whole stack:
   libcss/libdom/libhubbub/libparserutils/libwapcaplet/libnsfb/libnsgif/libnspsl/libnsutils) needs
   an x86_64 port before its dependent app builds. **vim BLOCKED on ncurses; dropbear on
   openssl/libtomcrypt+zlib; netsurf on its big lib stack; doom on fb-backend+libpng/zlib.** This is
   a multi-library porting program, each lib a real port (configure-cache + dllimport-shim + LP64).
2. **No x86_64 kext** — the PS/2 keyboard/mouse + e1000 kexts (`.nkext`) are i686-only, so the
   x86_64 system has NO interactive keyboard input yet (shells render but can't be typed into;
   verified apps via an exec-path PID-1 harness). Porting the kext loader + the 3 kexts to x86_64
   is needed for an interactive system (and for `ping`/networking the e1000 kext).

**REMAINING ports:** ping (inetutils — mostly libc, likely next-achievable), openssl (builds own
crypto, retarget Configure), then the lib-gated ones (vim/dropbear/netsurf/doom). Plus the x86_64
kexts (keyboard for interactivity, e1000 for net) and the final cut-over (delete `arch/x86`, drop
i686 — the user said 32-bit is no longer needed). Each `make <app>` target is now made arch-aware
on first port (the `nx-gcc`/`nx-port-build.sh` pattern). i686 green is NO LONGER a required gate
(user dropped dual-arch). (historical Plan-8 framing:)
**REMAINING — Plan 10 10c (the app ports, Tasks 7-16): a fresh multi-session tail.** Rebuild the
~12 apps against the now-working `x86_64-nanos` sysroot (external repos `~/Projects/nanos-sdk-work/
<app>-port`, `bash-nanos`, `netsurf-nanos`, via NanOS `make <app>` targets): nsh+coreutils → bash →
grep/vim/bzip2 → inetutils(ping) → openssl → dropbear → netsurf → doom → full disk image + smoke →
close. Each is a real external build (configure/CFLAGS triple swap + per-app LP64 fixes; OpenSSL
Configure retarget, NetSurf fb-offset audit, Doom int/pointer audit are the flagged ones). The
kernel + toolchain + libc are all ready; only the app ecosystem rebuild remains. (historical:)
Plan 7 T12-14 (deferred: widen `arch/mmu.h` to uintptr_t + flip the `-Wconversion` gate). And the
paging carry-forward gates [[nanos-x64-paging-carryforward]] (>1 GiB VA-window ceiling) now matter
since the real kernel uses full process address spaces — revisit before large-RAM/multiprocess.

**(historical) The gap that Plan 9 closed (was NOT in Plans 1-8):** the x86_64 kernel was STAGED.
The ring-3 proof used a MINIMAL inline syscall dispatch in KernelStage64 (write/read/open/close/
lseek/exit only) because the full `kernel/Syscall.cpp` Syscalls core + `kernel/Exec.cpp` pull
`Scheduler`/`Process`/`Signal`/the net stack/`DynLoader` — and those need x86_64 ports of the
i686-only MD pieces still missing: scheduler context switch (`switch.S`→switch64), `fork_x86`,
`sched_x86`, csprng entropy (`random_x86`), PCI/net (`pci_x86`). UN-STAGING the kernel (linking
the real `kernel/Kernel.cpp` via `make ARCH=x86_64 _all`) requires porting those ~4-5 MD
subsystems — effectively a "Plan 9". Only THEN can Plan 8 (SDK x86_64-nanos + picolibc + the ~12
app ports: bash/vim/grep/doom/netsurf/dropbear/...) proceed, since those apps need the full
POSIX syscall surface + fork/exec + pthreads + dynamic linking + networking. Plans 1-7 proved
every hard MECHANISM; the un-stage + app ports remain.

**POLICY 2026-06-19 — i686 is FROZEN; x86_64 is the SOLE development arch.** No new development,
ports, fixes, or features target i686. `arch/x86` + the i686 toolchain are KEPT (NOT deleted) only as
a frozen reference / differential-debugging oracle / fallback — never extended. i686 building or being
green is explicitly NOT a gate and NOT maintained; new in-tree code may assume x86_64. **Cut-over is
SPLIT:** flipping default `ARCH ?= x86_64` is cheap/reversible (do anytime); DELETING `arch/x86` is
one-way and destroys the oracle that found every MD bug this session (preempt/fault/signal = "mirror
i686") — DEFERRED until x86_64 burns in. The MI/MD split makes a dormant `arch/x86` cost nothing.
Before considering deletion: port i686-only helpers still needed on x64 (`nanologin`, `dhcpcfg`,
`nterm`), burn in `verify64`, close the >1 GiB VA ceiling.

**UPDATE 2026-06-19 — kexts work + TWO foundational stubs finished (live status doc:
`docs/superpowers/specs/2026-06-19-x86_64-honest-status-and-gap-analysis.md`).**
- The "No x86_64 kext / no interactive keyboard" gap above is now STALE: boot loads `kbd.nkext` +
  `mouse.nkext` + `e1000.nkext`; console keyboard works (drove bash via QEMU sendkey), SSH over e1000
  works, NanWM desktop (nwm + Files/Settings/Terminal/NetSurf) runs — see [[nanos-tls-ssl-ssh]].
- **PATTERN: the x64 port left several "lands when the scheduler is ported" stubs UNWIRED even after
  the scheduler was done.** Two were found+fixed this session, each a whole-system bug:
  1. **No involuntary preemption** — `irq64.S` returned with bare `iretq`, never calling `schedPreempt`.
     A busy-looping user task starved ksoftirqd-net + console → total wedge. Fixed (commit 75177a1).
  2. **Ring-3 CPU fault halted the machine** — `fault_x86_64.cpp` `for(;;) hlt`'d on every #GP/#PF; any
     app crash (NetSurf close) wedged the box. Fixed → `killCurrentProcess(SIGSEGV)` (commit 4fc541d).
  3. **No async signal delivery on IRQ/exception return to ring 3** — `Interrupt64.cpp` delivered
     signals only on syscall return; Ctrl+C/SIGKILL couldn't reach a CPU-bound ring-3 loop. Fixed →
     `signalDeliver` on ring-3 frame, like i686 (commit 8a57df3).
  AUDIT DONE: all three were the **ring-3-RETURN path** (Plan 6 wired the syscall return; the IRQ/
  exception/fault returns stayed Plan-1..4 debug stubs). Otherwise clean — per-task `%fs.base` TLS IS
  reloaded on switch (`Scheduler.cpp:170`); other `hlt` loops legit; `KernelStage64` is rescue-ISO only.
  Also fixed: `make run64`/`bringup64` booted the 64-bit image with `qemu-system-i386` →
  GRUB triple-fault loop (commit f099ab2). NanWM desktop apps now build for x64 (commit e76da9d).
- Remaining medium/low: dropbear `ssh host cmd` teardown linger (zombie + pipe never-EOF spin);
  nwm click-to-focus unverified on x64 (QEMU PS/2 imprecision); NetSurf #GP on close (app bug, now
  non-fatal). Cut-over still gated on Phase 2 (no `make ARCH=x86_64 test` in the routine gate).

Related: [[nanos-multiprocessing-roadmap]], [[nanos-sdk-and-vim-port]], [[no-shortcuts-on-foundations]]
