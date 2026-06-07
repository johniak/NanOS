# Stage 4 — fork / exec / wait / exit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** Unix process model — `fork` (eager copy), `execve` (loader into the process's
space), `waitpid` (reap zombies), `exit` (zombie). nsh becomes a fork/exec/wait shell.
Replaces synchronous `SYS_spawn`.

**Architecture:** Trap-frame + scheduler model (a process runs to `exit()`; syscalls
`iret`-return; blocking via `Scheduler::block/wake`). `Process` = scheduler `Task` +
address space + per-process fd table + pid/parent/exitCode + `ZOMBIE`. Per-process
`esp0`. Real `mmuFreeAddressSpace`. See `docs/superpowers/specs/2026-06-07-stage4-fork-exec-wait-design.md`.

**Branch/commits:** `dockerized-build`, commit per step, no Claude attribution. Steps
1–2 fully green; 3–6 are the cohesive model switch (`ls`/`cat` broken 3→5, back at 6).

**Key arch facts (from isr.S):** `Registers` IS the on-stack trap frame (low→high: ds,
edi,esi,ebp,esp,ebx,edx,ecx,eax, int_no, err, eip, cs, eflags, useresp, ss). The ISR
return tail (`pop eax; mov ds..; popa; add esp,8; iret`) restores it — factor a
`ret_from_fork` label there.

---

## Task 1: Per-process fd table + Process table (fully green)

**Files:** Create `kernel/Process.h`, `kernel/Process.cpp`, `tests/test_process.cpp`;
modify `kernel/Syscall.{h,cpp}`, `kernel/SyscallDispatch.cpp`, `Makefile`.

- [ ] **Step 1 — `kernel/Process.h`**: a `FdTable` + `Process` table.

```cpp
#ifndef PROCESS_H_
#define PROCESS_H_
#include "Scheduler.h"
namespace kernel {

struct Fd { bool used; bool isConsole; class String path; unsigned offset; unsigned size; };
// (reuse the existing Fd shape from Syscall.h; move it here)

struct FdTable {
    static const int MAXFD = 32;
    Fd fds[MAXFD];
    void initConsole();                  // 0/1/2 = console, rest free
    void copyFrom(const FdTable& o);     // fork: dup the parent's fds
};

struct Process {
    int pid;
    int parent;
    int exitCode;
    bool exited;
    Task* task;                          // scheduler task (0 for none)
    void* space;                         // arch::AddressSpace* (opaque here)
    FdTable fds;
};

class ProcTable {
public:
    static void init();
    static Process* alloc(int parent);   // a free slot, fresh pid
    static Process* current();           // process of the running task
    static Process* byPid(int pid);
    static void setCurrent(Process*);     // scheduler hook (or derive from current task)
};
}
#endif
```

- [ ] **Step 2 — failing test** `tests/test_process.cpp`: `FdTable::initConsole` sets
  0/1/2 console; `copyFrom` duplicates entries; allocating processes gives distinct pids.

- [ ] **Step 3 — implement** `kernel/Process.cpp` (a fixed `Process[MAXPROC]` array, pid
  counter; `current()` returns the process whose `task == Scheduler::current()`).

- [ ] **Step 4 — refactor `Syscall.{h,cpp}`**: remove the `Fd fds[]`/`exited`/`exitCode`
  members; `open/close/read/write/lseek/fstat/getdents64` operate on
  `ProcTable::current()->fds`; `exit/hasExited/code/resetForRun` operate on the current
  process. `Syscalls` keeps only `Vfs*` + console sink. Update `SyscallDispatch.cpp`
  accordingly.

- [ ] **Step 5 — wire the boot process**: `Kernel::start` (still Stage-3 flow) creates a
  `Process` for the init task so `ProcTable::current()` is valid before user code runs.

- [ ] **Step 6 — Makefile**: `MI_SOURCES += Process.o`; `TEST_MODULES += kernel/Process.cpp`
  (gate it only if coverage stays ≥90%; else leave ungated like Scheduler).

- [ ] **Step 7 — run** `make test`, `make check-arch`, `make build`, QEMU smoke (nsh/ls/cat
  unchanged). Commit `feat: per-process fd table + process table`.

---

## Task 2: mmuFreeAddressSpace (selective teardown) + host test

**Files:** modify `arch/include/arch/mmu.h`, `arch/x86/mm/mmu_x86.cpp`,
`arch/x86/mm/AddressSpace.{h,cpp}`; test `tests/test_addressspace.cpp`.

- [ ] **Step 1 — failing host test**: over the arena `PagingEnv`, build a space, map a few
  user-window pages, then `freeUserWindow(...)` frees exactly those frames + the user PT,
  and leaves kernel-half PDEs untouched. (Add an `AddressSpace::freeUserWindow(userVa)`
  that frees the user PDE's PT + its present frames, host-testable.)

- [ ] **Step 2 — implement** `AddressSpace::freeUserWindow(uint32_t userVa)`: for the PDE
  covering `userVa`, walk its PT, free each present frame, free the PT, clear the PDE.
  Then `arch::mmuFreeAddressSpace(space)`: `space->impl.freeUserWindow(0x400000)`, free the
  directory frame, `delete space`. Declare `mmuFreeAddressSpace` in `arch/mmu.h`.

- [ ] **Step 3 — run** `make test` (frame accounting), `make build`. Commit
  `feat: selective address-space teardown (mmuFreeAddressSpace)`.

---

## Task 3: Trap-frame model + execve (boots again to nsh$)

**Files:** modify `arch/include/arch/usermode.h`, `arch/x86/cpu/usermode_x86.cpp`,
`arch/x86/cpu/cpu_x86.cpp`, `arch/x86/cpu/syscall_x86.cpp`, `kernel/Scheduler.{h,cpp}`,
`kernel/Exec.{h,cpp}`, `kernel/SyscallDispatch.cpp`, `kernel/SyscallNr.h`, `kernel/Kernel.cpp`,
`arch/x86/cpu/isr.S` (ret_from_fork label, used later).

- [ ] **Step 1 — `archEnterUser`** in `usermode_x86.cpp` (the ring-3 entry, no longjmp):

```cpp
void archEnterUser(uint32_t entry, uint32_t userEsp, AddressSpace* space) {
    __asm__ __volatile__("cli");
    mmuSwitch(space);
    __asm__ __volatile__(
        "mov $0x23,%%ax; mov %%ax,%%ds; mov %%ax,%%es; mov %%ax,%%fs; mov %%ax,%%gs\n"
        "pushl $0x23; pushl %0; pushl $0x202; pushl $0x1B; pushl %1; iret"
        : : "r"(userEsp), "r"(entry) : "ax","memory");
}
```
Declare in `<arch/usermode.h>`. Remove `enterUser`/`userExit`/`spawnUserImage`/`g_userCtx`/
`g_depth`. Keep `execUserImage`'s mapping logic but split it: a `loadImageIntoSpace(...)`
that maps image+heap+stack and returns the user esp (no enterUser call).

- [ ] **Step 2 — esp0 per process**: `Scheduler` sets `arch::setKernelStack(current->esp0)`
  on each switch (and at `start()`); `Task` gains `unsigned esp0` (= kstack top). Remove the
  Stage-3 `g_kstacks[]`/`kstackTop(depth)` from `cpu_x86.cpp` (replaced by per-task esp0).

- [ ] **Step 3 — `exit` → zombie**: `syscall_x86.cpp` syscall trap: after dispatch, if
  `current process hasExited()`, call `kernel::procExit()` which frees the address space
  (`mmuFreeAddressSpace`), sets the task `TASK_ZOMBIE`, wakes the parent, and
  `Scheduler::schedule()`s away (never returns). No `userExit`/longjmp.

- [ ] **Step 4 — `execve` (number `SYS_execve 11`)** in `kernel/Exec.cpp` +
  `SyscallDispatch.cpp`: stage the `.nxe` (under kernel CR3), free the caller's user frames,
  load the image+heap+stack into the process's space, build argv, then **rewrite the live
  trap frame** (`regs->eip = entry; regs->useresp = esp; regs->eax = 0; cs/ss/ds = user
  selectors; eflags = 0x202`) so the syscall `iret`s into the new program. The dispatch
  passes `Registers*` to the execve handler for this rewrite.

- [ ] **Step 5 — PID 1 via the new model**: `Kernel.cpp` init task body =
  `loadImageIntoSpace(init.nxe) ; archEnterUser(entry, esp, space)`. `init.nxe` `execve`s
  `/disks/main/nanos/bin/nsh.nxe`.

- [ ] **Step 6 — build + QEMU**: boots to `nsh$`; `echo`/`exit` work; `cat /proc/uptime`
  advances. (`ls`/`cat` broken until Task 6 — `nsh` still calls the now-removed spawn; so in
  THIS step temporarily make `nsh`'s spawn path print "not yet" or stub, OR leave the spawn
  syscall returning -ENOSYS — the shell prints command-not-found.) Commit
  `feat: trap-frame process model + execve; PID1 execs nsh`.

---

## Task 4: fork + ret_from_fork

**Files:** modify `arch/x86/cpu/isr.S` (label), create `arch/x86/cpu/fork_x86.cpp` (or in
usermode_x86), `kernel/Process.cpp` (fork glue), `SyscallDispatch.cpp`, `SyscallNr.h`.

- [ ] **Step 1 — `ret_from_fork` label** in `isr.S`: add `[GLOBAL ret_from_fork]` at the
  `pop eax` line (start of the return tail). It restores ds/segs, `popa`, `add esp,8`, `iret`.

- [ ] **Step 2 — `archForkChild(childTask, parentRegs, childSpace)`** (arch): fabricate the
  child kernel stack: copy `*parentRegs` (a `Registers`) to the top with `eax=0`; below it a
  context-switch save frame `[cr3=childSpace][ebp=0][edi=0][esi=0][ebx=0][ret=ret_from_fork]`;
  set `childTask->kesp` to the cr3 slot. (Matches `archContextSwitch`'s pop order, then
  `ret_from_fork` iret's the copied frame.)

- [ ] **Step 3 — `fork()` (`SYS_fork 2`)** in `Process.cpp` + dispatch (passes `Registers*`):
  `child = ProcTable::alloc(currentPid)`; `child->space = mmuCopyAddressSpace(parent->space)`
  (new dir + copy each user-window frame); `child->fds.copyFrom(parent->fds)`; create the
  child scheduler task with `archForkChild(child->task, regs, child->space)`, `TASK_READY`;
  return `child->pid` to the parent (`regs->eax = pid`). Add `mmuCopyAddressSpace` (arch:
  new space + for each present user-window frame, alloc + memcpy + map).

- [ ] **Step 4 — QEMU fork test**: a tiny user program (or init) `int p=fork(); if(!p)
  write(1,"child\n"); else write(1,"parent\n");`. Both lines appear; no faults. Commit
  `feat: fork (eager copy) + ret_from_fork`.

---

## Task 5: waitpid + zombie reap

**Files:** modify `kernel/Process.cpp`, `SyscallDispatch.cpp`, `SyscallNr.h`,
`arch/x86/cpu/syscall_x86.cpp` (exit wakes waiter), `tests/test_process.cpp`.

- [ ] **Step 1 — failing host test**: zombie/wait state machine — `procExit(pid,code)` →
  ZOMBIE+code; `reapChild(parent)` finds a zombie child, returns pid+code, frees the slot;
  none → `-ECHILD`.

- [ ] **Step 2 — `waitpid` (`SYS_waitpid 7`)**: block (set current BLOCKED, remember it as
  the parent waiter) until a child of `current` is `TASK_ZOMBIE`; copy `exitCode` to
  `*status`; free the child's kstack + slot; return its pid. `exit` (Task 3 step 3) already
  frees the space + sets ZOMBIE; now also `Scheduler::wake(parentWaiter)`.

- [ ] **Step 3 — run** host test; QEMU: the fork test program with the parent `waitpid`-ing
  prints "parent: child N exited C". Commit `feat: waitpid + zombie reap`.

---

## Task 6: nsh → fork/exec/wait; remove spawn

**Files:** modify `user/libc-glue/syscalls.c` (add `fork`/`execve`/`waitpid`; drop `spawn`),
`user/nsh.c`, `kernel/SyscallDispatch.cpp` (drop `SYS_spawn`), remove `spawnProgram`/
`spawnUserImage` remnants.

- [ ] **Step 1 — glue wrappers** in `user/libc-glue/syscalls.c`:
  `int fork(void){return sys3(SYS_fork,0,0,0);}`,
  `int execve(const char*p,char*const argv[],char*const envp[]){return sys3(SYS_execve,(int)p,(int)argv,(int)envp);}`,
  `int waitpid(int pid,int*st,int o){return sys3(SYS_waitpid,pid,(int)st,o);}`. Remove `spawn`.

- [ ] **Step 2 — nsh** runs commands via:
```c
int pid = fork();
if (pid == 0) { char* ev[]={0}; execve(path, argv, ev); _exit(127); }
else if (pid > 0) { int st; waitpid(pid, &st, 0); }
else printf("nsh: fork failed\n");
```
Keep builtins/bare-ls; keep the `termmode` cooked/raw toggle around the child.

- [ ] **Step 3 — remove** `SYS_spawn` from `SyscallNr.h`/dispatch and the leftover
  `spawnProgram`/`spawnUserImage` declarations/defs.

- [ ] **Step 4 — build + check-arch + test + QEMU**: `ls -l /disks/main/boot`, `cat`,
  `echo`, unknown cmd, run ~10 commands in a row (no RAM exhaustion → teardown works),
  `cat /proc/uptime` advances, no `v=08/0d/0e` faults. Commit
  `feat: nsh uses fork/exec/wait; remove SYS_spawn`.

- [ ] **Step 5 — ROADMAP** Stage 4 ✅; update memory.

---

## Self-review
- Spec coverage: fd-table (T1), teardown (T2), trap-frame+execve+exit-zombie (T3), fork (T4),
  waitpid (T5), nsh+remove-spawn (T6). All mapped.
- Risk: `ret_from_fork` child-stack layout must match `archContextSwitch` pop order then the
  ISR return tail (T4 step 2) — triple-fault if off; verify with the fork test program before
  nsh. Teardown must not free shared kernel PTs (T2). esp0-per-process required once >1 ring-3
  task (T3 step 2).
- The 3→5 window where `ls`/`cat` are broken is called out in the spec; T3 step 6 stubs the
  shell's spawn path so the shell still runs.
