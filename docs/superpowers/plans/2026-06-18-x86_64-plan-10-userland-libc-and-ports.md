# x86_64 Plan 10 — Full userland libc (pthread/TLS) + the app ports (complete the migration)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the x86_64 migration end-to-end: build the **full 64-bit `libc.ndl`/`libc.a`** (picolibc + the musl pthread/TLS glue, not just the minimal freestanding `init.nxe` Plan 6 shipped), prove it with a libc + pthread `hello`/TLS smoke in ring 3, then **rebuild the ~12 external app ports** against the `x86_64-nanos` sysroot so the x86_64 disk image boots and runs `nsh`/`bash` plus a representative tool set (`ls`/`cat`/`grep`/`vim`, `ping`, `openssl`, an SSH/HTTP server, Doom/NetSurf). After this plan the x86_64 port is at functional parity with i686.

**Architecture:** The x86_64 KERNEL migration is complete (Plans 1-7 + 9: real `kernel/Kernel.cpp` boots from disk, scheduler + ring-3 + ext RW + PCI + kexts; `make ARCH=x86_64 _all`/`make image64`). The `x86_64-nanos` cross-toolchain is built (Plan 8 8b in `~/Projects/nanos-sdk`: binutils 2.43 + gcc 14.2.0 + picolibc 1.8.6, ELF64, LINK_SPEC = decision #1). What remains is the **userland C library** and the **app ecosystem** — this plan. The lever is the spec's **4-artifact contract** (`<triple>/include` LP64 headers + x86_64 `SyscallNr.h`, and `<triple>/lib/{libc.a,libc.ndl}`): once those are genuinely 64-bit, every port rebuilds by recompilation. The blocker Plan 8 8b hit: the userland **musl pthread/TLS arch layer is i386-only** and the NanOS `Makefile` `libc.ndl` rules are hard-wired 32-bit — 10a fixes exactly that.

**Tech Stack:** `x86_64-nanos-` toolchain (Plan 8), picolibc 1.8.6 x86_64, NanOS `Makefile` (`bin/libc.ndl{,.a}` + `crt0`/`nxhdr`), `mknx64` + `arch/x86_64/user-nx.ld` (Plan 6), external port repos (`~/Projects/nanos-sdk-work/<app>-port`, `~/Projects/bash-nanos`, `~/Projects/netsurf-nanos`) driven via NanOS `make <app>` targets, `qemu-system-x86_64 -drive` + headless screendump.

### Decisions baked in
- **#1 (unchanged, decision #1 of the migration):** user load base `0x800000` (low 2 GiB), small/non-PIC, `R_X86_64_64`(+`R_X86_64_32S`) relocations — already consistent in `mknx64` + `user-nx.ld` + the SDK `LINK_SPEC`. This plan does not change the model.
- **#2 — TLS via `%fs.base` + `arch_prctl`.** i686 keeps the thread pointer in a `%gs` GDT entry set by `set_thread_area`; x86_64 keeps it in `%fs.base` set by `arch_prctl(ARCH_SET_FS, ptr)` (the kernel side — the `IA32_FS_BASE` MSR + the `arch_prctl` syscall — is done in Plan 6 / Plan 9; this plan does the USERLAND side: the musl `pthread_arch.h` thread-pointer read via `%fs`, the TCB layout, and the crt0 main-thread bootstrap calling `arch_prctl`). See [[nanos-pthread-port]] for the i686 musl-on-picolibc port this mirrors.
- **#3 — SSE on for userland** (SysV ABI; already set for x86_64 userland in Plan 6/8). Kernel stays `-mno-sse`.
- **#4 — `SYS_arch_prctl` (158)** must be handled by the kernel syscall dispatch for the userland TLS bootstrap to work — verify it routes to setting `%fs.base` (Plan 6 added the number; confirm the dispatch sets `IA32_FS_BASE`). If missing, wire it (small kernel-side fix, noted in Task 3).

### Reference
- Plan 8 (`2026-06-15-x86_64-plan-8-sdk-app-ports.md`) — the original 8a/8b/8c breakdown + the port list + external-repo mechanics. This plan SUPERSEDES its 8a-libc assumption (which was unmet) and continues 8c.
- i686 templates: `user/libc-glue/pthread/include/{atomic_arch.h,pthread_arch.h,syscall_arch.h}`, `user/libc-glue/include/nx-tcb.h`, `user/crt0.S` (the i686 TLS bootstrap), the NanOS `Makefile` `libc.ndl`/`USER_GLUE`/`DYN_GLUE` rules + the `make <app>` targets (lines ~89-265).
- musl official x86_64 arch headers (`arch/x86_64/atomic_arch.h`, `pthread_arch.h`, `syscall_arch.h`) are the authoritative source for the 64-bit asm.

---

## File Structure

| Plik | Odpowiedzialność | Akcja |
|---|---|---|
| `user/libc-glue/pthread/include/atomic_arch.h` | make arch-conditional: x86_64 atomics (musl x86_64 `a_cas`/`a_cas_p`/`a_swap`/… on 64-bit regs) under `#ifdef __x86_64__`, keep i386 under `#else` | Modify |
| `user/libc-glue/pthread/include/pthread_arch.h` | x86_64 thread-pointer: `__get_tp()` reads `%fs:0` (was `%gs:0`); TP→TCB arithmetic for the x86_64 layout | Modify |
| `user/libc-glue/pthread/include/syscall_arch.h` | x86_64 `__syscall0..6` via the `syscall` instruction (args `rdi/rsi/rdx/r10/r8/r9`, clobber `rcx`/`r11`), under `#ifdef __x86_64__` | Modify |
| `user/libc-glue/include/nx-tcb.h` | TCB field widths/offsets → 64-bit (pointers 8 B) under `#ifdef __x86_64__`; keep i386 layout under `#else` | Modify |
| `user/crt064.S` (or `crt0.S` arch-switched) | main-thread TLS bootstrap: `arch_prctl(ARCH_SET_FS, tcb)` instead of i686 `set_thread_area`; SysV `_start` already in `crt064.S` (Plan 6) — add the TLS init before `main` | Modify |
| `Makefile` | make the `libc.ndl`/`libc.ndl.a`/`USER_GLUE`/import-lib rules **arch-aware**: under `ARCH=x86_64` use `$(MKNX64)`, `nasm -f elf64`, the x86_64 crt0/nxhdr, and the 64-bit libc-glue objects → produce 64-bit `bin/libc.ndl{,.a}` + `bin/crt0.o`; the `make <app>` targets copy into `$(SDK_TC)/x86_64-nanos/lib/` (not i686-nanos) when `ARCH=x86_64` | Modify |
| (external) `~/Projects/<app>-port/*`, `~/Projects/bash-nanos`, `~/Projects/netsurf-nanos` | retarget each port build to `x86_64-nanos` (configure/CFLAGS triple swap; LP64 fixes where a port assumed ILP32) | Modify (in those repos) |

---

## Sub-stream 10a — the full 64-bit libc.ndl

## Task 1: x86_64 atomics — `atomic_arch.h`

**Files:** Modify `user/libc-glue/pthread/include/atomic_arch.h`.

- [ ] **Step 1:** Read the current (i386) `atomic_arch.h` and musl's official `arch/x86_64/atomic_arch.h`. The i386 file uses 32-bit ops (`%esp`-relative, `a_cas_p` truncates a pointer to 32-bit). Wrap the file: `#if defined(__x86_64__)` → the musl x86_64 atomics (`a_cas` on `int` via `lock cmpxchg`, `a_cas_p` on `void*` via `lock cmpxchg` on a 64-bit reg, `a_swap`, `a_fetch_add`, `a_and`/`a_or`, `a_spin`, `a_barrier` using 64-bit register operands), `#else` → the existing i386 body verbatim. Keep `a_ctz_*`/`a_clz_*` portable forms if the file has them.
- [ ] **Step 2:** Syntax-check both arches:
```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build sh -c '
  echo "#include \"atomic_arch.h\"" > /tmp/a.c
  i686-elf-gcc   -fsyntax-only -I user/libc-glue/pthread/include /tmp/a.c &&
  x86_64-elf-gcc -fsyntax-only -I user/libc-glue/pthread/include /tmp/a.c && echo OK'
```
Expected: `OK` (a bare include may need the musl internal context; if so, compile a tiny TU that `a_cas`-es an int — adapt to the file's include deps, the point is both arches accept the x86_64/i386 branches).
- [ ] **Step 3:** Commit. `git add user/libc-glue/pthread/include/atomic_arch.h` → `x86_64: musl atomic_arch for x86_64 (64-bit lock cmpxchg/xchg), keep i386`.

## Task 2: x86_64 thread pointer + syscalls + TCB — `pthread_arch.h`, `syscall_arch.h`, `nx-tcb.h`

**Files:** Modify the three headers.

- [ ] **Step 1:** `pthread_arch.h` — under `#if defined(__x86_64__)`, `__get_tp()` reads `%fs:0` (`__asm__("mov %%fs:0,%0" : "=r"(tp))`); the TP-to-`pthread` arithmetic matches musl's x86_64 convention (TP points at the end/TCB head per the x86_64 TLS variant II). Keep i386 (`%gs:0`) under `#else`. Mirror musl `arch/x86_64/pthread_arch.h`.
- [ ] **Step 2:** `syscall_arch.h` — under `#if defined(__x86_64__)`, `__syscall0..6` via the `syscall` instruction: nr in `rax`, args `rdi/rsi/rdx/r10/r8/r9`, result `rax`, clobbers `rcx`,`r11`,`memory`. Mirror musl `arch/x86_64/syscall_arch.h`. Keep i386 (`int $0x80`) under `#else`.
- [ ] **Step 3:** `nx-tcb.h` — make the TCB struct field widths 64-bit under `#if defined(__x86_64__)` (pointers/`self`/dtv are 8 B; the layout must match what `pthread_arch.h` + crt0 assume — x86_64 TLS variant II: the TCB head holds `self` at offset 0 so `%fs:0` == the thread pointer). Keep i386 layout under `#else`.
- [ ] **Step 4:** Both-arch syntax check (as Task 1 Step 2, including all three headers).
- [ ] **Step 5:** Commit. `git add` the three → `x86_64: musl pthread_arch (%fs TP) + syscall_arch (syscall insn) + 64-bit TCB`.

## Task 3: crt0 main-thread TLS bootstrap + `arch_prctl`

**Files:** Modify `user/crt064.S` (the Plan-6 x86_64 crt0); verify the kernel handles `SYS_arch_prctl`.

- [ ] **Step 1:** Read i686 `user/crt0.S` — its `_start` calls a `__nx_init_tls` that does `set_thread_area` to point `%gs:0` at the TCB before `main` (so `errno`/stdio TLS work). In `crt064.S`, add the equivalent BEFORE calling `main`: allocate/locate the main-thread TCB (static reservation or a tiny brk), then `arch_prctl(ARCH_SET_FS=0x1002, tcb)` via the `syscall` instruction (nr `SYS_arch_prctl`=158, arg0 `rdi`=0x1002, arg1 `rsi`=tcb). The TCB's offset-0 word must be `self` (point it at itself) so `%fs:0` reads the thread pointer (variant II). Mirror the i686 logic, x86_64 register/ABI.
- [ ] **Step 2: Verify the kernel sets `%fs.base` on `arch_prctl`.** Check `kernel/SyscallDispatch.cpp` (or the syscall core) routes `SYS_arch_prctl(ARCH_SET_FS, ptr)` → `arch::archSetUserFsBase(ptr)` (the `IA32_FS_BASE` MSR setter from Plan 6 `usermode_x86_64.cpp`). If it does NOT, wire it (a small `case SYS_arch_prctl:` in the dispatch calling `archSetUserFsBase` + storing it in the thread for context-switch reload). This is the one possible kernel-side fix; note it. (i686 has no arch_prctl — guard with `#if`/the x86_64 syscall number.)
- [ ] **Step 3:** Compile `crt064.S` (NASM/`as` per its format) for x86_64. Expected: assembles.
- [ ] **Step 4:** Commit. `git add user/crt064.S` (+ `kernel/SyscallDispatch.cpp` if wired) → `x86_64: crt0 main-thread TLS bootstrap via arch_prctl(ARCH_SET_FS)`.

## Task 4: arch-aware `libc.ndl` / glue Makefile rules → produce the 64-bit artifacts

**Files:** Modify `Makefile`.

- [ ] **Step 1:** Read the current `libc.ndl`/`libc.ndl.a`/`USER_GLUE`/`DYN_GLUE`/import-lib rules (the i686 chain that builds `bin/libc.ndl{,.a}` + `bin/crt0.o`) and the existing `ifeq ($(ARCH),x86_64)` userland block (which today builds only the minimal `init.nxe`).
- [ ] **Step 2:** Make the libc-glue + libc.ndl build **arch-aware**: under `ARCH=x86_64`, build the libc-glue objects with the x86_64 userland flags (SSE on, `-mcmodel=small -fno-pic`, the 64-bit pthread/TLS headers from Tasks 1-3), assemble `.s`/`.S` with `nasm -f elf64`, link the `.ndl` with `$(MKNX64)` + `arch/x86_64/user-nx.ld`, and emit `bin/libc.ndl` + `bin/libc.ndl.a` + the x86_64 `bin/crt0.o`/`bin/nxhdr.o` as ELF64. Keep the i686 chain unchanged under the `#else`/default branch.
- [ ] **Step 3: Produce + verify the artifacts.**
```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a
docker run --rm -v "$(pwd)":/src -w /src nanos-build sh -c '
  for f in bin/libc.ndl bin/libc.ndl.a bin/crt0.o; do echo "== $f =="; x86_64-elf-readelf -h $f | grep -E "Class|Machine"; done'
```
Expected: all three `Class: ELF64`, `Machine: X86-64`. `libc.ndl` exports the libc symbols; `libc.ndl.a` is the import library.
- [ ] **Step 4: i686 regression.** `make build && make check-arch && make test` — i686 still builds its 32-bit libc.ndl + the host suite is green.
- [ ] **Step 5:** Commit. `git add Makefile` → `build: arch-aware libc.ndl/crt0 — produce the 64-bit userland C library`.

---

## Sub-stream 10b — prove the contract

## Task 5: inject the 4 artifacts into the `x86_64-nanos` sysroot + libc `hello.nxe` smoke

**Files:** Modify the NanOS `make` plumbing (the sysroot-copy step) — mirror the i686 `cp ... $(SDK_TC)/i686-nanos/lib/` for `x86_64-nanos`.

- [ ] **Step 1:** Make the sysroot-injection (the `cp bin/libc.ndl{,.a}` + `crt0.o`/`nxhdr.o` + the LP64 headers + `SyscallNr.h` into `$(SDK_TC)/<triple>/lib` & `/include`) target `x86_64-nanos` when `ARCH=x86_64`. (This is the Plan-8 Task-6 injection, now feasible because the artifacts exist.)
- [ ] **Step 2: Build a libc `hello.nxe` with `x86_64-nanos-gcc`** (picolibc `printf`, NOT the freestanding path) → link against `libc.ndl` → `mknx64` → v4 `.nxe`. Confirm `nm -u hello.nxe` resolves `write`/`_exit`/etc. through the import lib (no undefined).
- [ ] **Step 3: QEMU smoke** — copy `hello.nxe` into the disk image at the path the kernel can exec (or have `init.nxe`/`nsh` launch it), boot `qemu-system-x86_64 -drive`, screendump → expect `hello x86_64-nanos`. Zero faults. **This closes the 4-artifact contract: a real libc program runs in ring 3.**
- [ ] **Step 4:** Commit (NanOS plumbing) → `build: inject x86_64-nanos sysroot artifacts; libc hello.nxe smoke`.

## Task 6: pthread / TLS smoke (errno + a thread)

**Files:** a tiny test program (in-tree under `user/` or a port-repo smoke).

- [ ] **Step 1:** Build a `pthread_hello` that (a) touches `errno` (proves `%fs.base` TLS works — the original fault Plan 6 #2 was about), and (b) `pthread_create`s one thread that prints, joins it. Link against `libc.ndl` (musl pthread on picolibc, now x86_64).
- [ ] **Step 2: QEMU smoke** — boot, run it (via `nsh`/`init` once a shell exists, or as the exec'd program), expect both the errno-based output and the thread's message, zero faults. Proves the pthread/TLS arch port (Tasks 1-3) at runtime.
- [ ] **Step 3:** Commit any in-tree test + note the result.

---

## Sub-stream 10c — the app ports (external repos, via `make <app>`)

> Each [PORT] task runs in the external repo and is invoked from NanOS via the `make <app>` target (which mounts the port tree into the container, builds against the `x86_64-nanos` sysroot, and emits the `.nxe`). The retarget per port = swap the build triple `i686-nanos` → `x86_64-nanos` in its configure/CFLAGS, rebuild, and fix any LP64 assumption the port made (most are clean recompiles; the spec flags OpenSSL/Doom/NetSurf as the ones needing care). Verification ([NANOS]) = copy the `.nxe` into `bin/`, `make image64`, headless QEMU boot, run it. Commit in the port repo (clean messages, no AI attribution) + the NanOS-side image wiring here. Order = by risk.

## Task 7: [PORT:nsh + coreutils] the shell + ls/cat/grep/echo

- [ ] Retarget the in-tree `nsh` + the sbase coreutils (these are in-tree `user/`, not external — rebuild them for x86_64 like `init.nxe`: arch-aware Makefile target). Boot the x86_64 image, confirm `nsh` launches as the shell and `ls`/`cat` work over `/disks/main`. (This is the first interactive milestone — a 64-bit shell.) Gate: QEMU shows an `nsh` prompt + a working `ls`. Commit.

## Task 8: [PORT:bash] bash (external `~/Projects/bash-nanos`)

- [ ] `make bash` retargeted to x86_64-nanos (the bash fork + its `nanos-build.sh`). Fix LP64 if any. Gate: `bash` runs in QEMU (prompt + a builtin). Commit in `bash-nanos` + NanOS image wiring.

## Task 9: [PORT] grep + vim + bzip2 (clean recompiles)

- [ ] `make grep` / `make vim` / `make bzip2` (or their equivalents) retargeted. These are flagged as clean recompiles. Gate: each runs in QEMU (`grep` a file, `vim` opens, `bzip2` round-trips). Commit per port repo + image wiring.

## Task 10: [PORT:inetutils] networking tools (ping/wget/telnetd/inetd/ifconfig/traceroute)

- [ ] `make ping` (+ the inetutils family) retargeted to x86_64-nanos. Depends on the kernel net stack (done — e1000 enumerated in Plan 9). Gate: `ping` the gateway / a host in QEMU (with `-netdev`), reply received. Commit.

## Task 11: [PORT:openssl] OpenSSL (retarget Configure)

- [ ] `make openssl` — retarget OpenSSL's `Configure` target to the x86_64-nanos triple (spec §5.4 #4: it needs an explicit Configure target, not just CFLAGS). Gate: `openssl version` + a hash/`s_client`-less crypto self-test in QEMU. Commit.

## Task 12: [PORT:dropbear] Dropbear SSH (depends on OpenSSL/libtomcrypt)

- [ ] `make dropbear` retargeted. Depends on Task 11's crypto. Gate: the SSH server starts in QEMU and accepts a connection (or at least initializes its host key — match the i686 smoke). Commit.

## Task 13: [PORT:netsurf] NetSurf + libnsfb (framebuffer offset audit)

- [ ] `make netsurf` (external `~/Projects/netsurf-nanos` + the libnsfb NanWM backend) retargeted. Spec §5.4 #5: audit the framebuffer byte-offset arithmetic for LP64. Depends on the kernel framebuffer/NanWM (present). Gate: NetSurf renders a page (or its window) in QEMU. Commit.

## Task 14: [PORT:doom] Doom (int-vs-pointer audit)

- [ ] `make doom` retargeted; audit the port's int-vs-pointer assumptions (spec §5.4 #5). Depends on framebuffer + the IWAD on `/disks/main/apps/doom`. Gate: Doom reaches its title screen in QEMU. Commit.

## Task 15: [NANOS] full image integration + whole-system QEMU smoke

- [ ] Assemble the x86_64 disk image with all the rebuilt `.nxe` (the `/nanos/bin` system utils + the `/apps` bundles + the `/bin` symlink farm — mirror the i686 `_image` population for x86_64). Boot headless, drive a short scripted session (launch `nsh` → `ls` → `cat` → run one app), screendump, confirm zero faults and e2fsck-clean. This is the parity milestone.
- [ ] Commit the NanOS image-population wiring → `build: x86_64 full disk image — nsh + coreutils + app bundles`.

## Task 16: [NANOS] close the migration

- [ ] **Step 1:** Full regression: `make build && make check-arch && make test && make ARCH=x86_64 test` — all green.
- [ ] **Step 2:** Re-confirm the x86_64 full-image boot once more from clean.
- [ ] **Step 3:** Update the migration memory: x86_64 at functional parity (kernel + userland libc + apps). Per the migration spec §0.1, the cut-over step (DELETE `arch/x86`, drop the `i686-*` toolchains, ship only `x86_64-nanos` + `.nxe` v4) is the FINAL act — flag it as a separate, deliberate decision for the user (do NOT delete i686 in this plan unless explicitly told; keeping both green has been the whole-migration invariant).
- [ ] **Step 4:** Commit (close marker) → `x86_64: migration complete — userland libc + app ecosystem at i686 parity`.

---

## Self-Review

- **Spec coverage:** closes the gap Plan 8 8b hit (full `libc.ndl` for x86_64) via the musl pthread/TLS arch-layer port (Tasks 1-3) + arch-aware libc.ndl rules (Task 4), proves it (Tasks 5-6), then completes Plan 8 8c (the app ports, Tasks 7-14) + integration (Task 15) + close (Task 16). Decisions #1/#3 unchanged from earlier plans; #2 (`%fs.base`/`arch_prctl`) is the userland half of Plan 6's kernel-side TLS; #4 routes `SYS_arch_prctl` (verify/wire in Task 3).
- **Placeholders:** the pthread/TLS port (Tasks 1-3) names the exact files + the musl x86_64 upstream as the authoritative source for the asm + the i686 in-tree file as the structural template (the same mechanical-port form Plan 9 used successfully). The app ports (10c) are recompiles against the new sysroot, invoked via existing `make <app>` targets — the per-port "code" is the triple retarget + any LP64 fix, with a QEMU run gate each.
- **Type/naming consistency:** arch-conditional (`#if defined(__x86_64__)`) headers keep i686 green; `mknx64`/`user-nx.ld`/`crt064.S` are the Plan-6 x86_64 names; sysroot triple `x86_64-nanos` mirrors `i686-nanos`; `bin/libc.ndl{,.a}` are produced as ELF64 under `ARCH=x86_64`.
- **Risk:** Tasks 1-3 (pthread/TLS arch port) are the novel risk — a wrong TCB offset or `%fs` setup faults on the first `errno`/thread access; Task 6's pthread smoke is the runtime gate. The app ports' risk is per-app (OpenSSL Configure, NetSurf fb offsets, Doom int/pointer) — each isolated behind its own QEMU gate. External-repo work (10c) needs the port repos present (`~/Projects/...`) and is long (each port = a real build); pace accordingly.
- **i686 green throughout:** every header change is arch-conditional; the Makefile libc.ndl change is under `ARCH=x86_64`; verified in Task 4 Step 4 + Task 16.

## Zależności

- **Requires Plans 1-9** (kernel + toolchain): the real x86_64 kernel boots with full syscalls/fork/exec/scheduler/net/kexts + ring-3 (`%fs.base`/`arch_prctl` kernel side), and the `x86_64-nanos` toolchain + picolibc exist in `~/Projects/nanos-sdk`.
- **External repos must be present:** `~/Projects/nanos-sdk`, `~/Projects/nanos-sdk-work/<app>-port`, `~/Projects/bash-nanos`, `~/Projects/netsurf-nanos`. 10c is invoked via the NanOS `make <app>` targets.
- Work continues on `feat/x86_64-foundation` (NanOS, local/unpushed) + the port repos' own branches. No mention of Claude/AI in any commit. The i686→x86_64 cut-over (deleting `arch/x86`) is deferred to an explicit final decision (Task 16 Step 3).
