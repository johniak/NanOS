# NanOS — Stage 4: fork / exec / wait / exit (Unix process model)

_Design spec. Date: 2026-06-07. Branch: `dockerized-build`. Roadmap: Stage 4._

## Goal

Replace the synchronous `SYS_spawn` with the real Unix process model: `fork` (eager
address-space copy), `execve` (reuse the loader into the process's space), `waitpid`
(block for a child, reap the zombie), `exit` (become a zombie). The shell becomes a
proper `fork`+`exec`+`wait` shell. Foreground only (no `&` jobs — trivial later).

This is the culmination of the multiprocessing roadmap: several ring-3 processes run
concurrently under the Stage 3 scheduler, each with its own address space and fd table.

## The model shift (the heart of Stage 4)

Today a program runs **synchronously**: `enterUser` (iret to ring 3) wrapped in
`nx_setjmp(g_userCtx)`, and `exit` does `nx_longjmp` back; nested `spawnUserImage`
runs a child to completion. This only supports ONE ring-3 program at a time.

Stage 4 switches to the **trap-frame + scheduler** model:
- A process is a scheduler task that runs until `exit()`. A syscall returns by
  `iret`-ing the trap frame (`Registers`) on the process's kernel stack.
- Blocking syscalls (`waitpid`, `read`) call `Scheduler::block()`; they are woken via
  `Scheduler::wake()` (keyboard IRQ, or a child exiting).
- `g_userCtx` / `nx_longjmp` / `spawnUserImage` / the nested `g_depth` machinery are
  **removed**.
- New arch primitive `archEnterUser(entry, userEsp, space)`: `cli`; switch CR3; `iret`
  to ring 3. It does NOT return (the process runs until it exits).

## Components

### Process (extends the scheduler Task)

```cpp
struct Process {
    Task task;                 // scheduler context (kesp, state, kstack)  [or Task gains these]
    int pid;
    int parent;                // pid of the parent (for waitpid / orphan handling)
    arch::AddressSpace* space; // its user address space (0 for kernel threads)
    unsigned esp0;             // top of its kernel stack (TSS.esp0 when it runs)
    int exitCode;              // valid once ZOMBIE
    FdTable fds;               // per-process file descriptors (see below)
};
```

A process table indexed by pid. Task states gain **`TASK_ZOMBIE`** (exited, awaiting
reap). The scheduler sets `TSS.esp0 = current->esp0` on every switch (Stage 3 left
`esp0` global because only one ring-3 task existed; now it is per-process).

### Per-process fd table

The fd array moves out of the single global `Syscalls` into `Process::fds`. The
`Syscalls` methods (`open/read/write/lseek/fstat/getdents64/close`) operate on the
**current process's** `fds` (via `Scheduler::current()`); the `Vfs*` and the console
write sink stay global. `exitCode`/`exited` also become per-process. `fork` copies the
parent's `FdTable` to the child (fds 0/1/2 inherited).

`FdTable` is a small struct: `{ Fd fds[MAXFD]; }` with the open/close/read/write logic
in `Syscalls` reading `currentProcess()->fds`.

### Address-space teardown (new — required)

`mmuDestroyAddressSpace` currently **leaks**. With `fork`/`exec`/`exit` per command,
leaking a whole address space each time would exhaust RAM fast. Stage 4 implements
**selective free**: free the USER-window frames `[0x400000, 0x500000)`, the private
user page table, and the directory frame — but NOT the shared kernel-half page tables
(aliased via `adoptKernelDirectory`). New: `mmuFreeAddressSpace(space)` (arch), walking
only the user-window PDE.

## Syscalls (Linux i386 numbers)

| nr | call | behaviour |
|----|------|-----------|
| 2  | `fork()` | Eager copy. New `Process` + new address space (`mmuCreateAddressSpace`); copy each mapped user-window frame (new frame + memcpy); copy the `FdTable`; copy the current syscall trap frame into the child's kernel stack with **`eax=0`**; set the child's kernel stack so the scheduler resumes it into a `ret_from_fork` trampoline that `iret`s that frame. Child added `TASK_READY`. Parent returns the child pid. |
| 11 | `execve(path, argv)` | Free the current process's user frames; load `.nxe` into the (reset) address space; build the SysV stack/argv; **rewrite the current trap frame** (`eip=entry`, `useresp=esp`, segments) so the syscall `iret`s into the new program. Does not return on success; `-ENOENT` etc. on failure. |
| 7  | `waitpid(pid, *status, opts)` | Block (deschedule) until a child of this process is `TASK_ZOMBIE`; copy its exit code to `*status`; reap it (free its kstack + process slot); return its pid. |
| 1  | `exit(code)` | Set `exitCode`; free the address space (`mmuFreeAddressSpace`); mark `TASK_ZOMBIE`; wake the parent if it is waiting; `schedule()` away permanently. |

`SYS_spawn` is removed (and the `spawnUserImage`/nested machinery with it).

### `ret_from_fork` (child first-run)

The child's kernel stack is fabricated (low→high address):
`[cr3][ebp=0][edi=0][esi=0][ebx=0][ret=ret_from_fork] [ds][edi..eax(=0)][int_no][err][eip][cs][eflags][useresp][ss]`
- `kesp` points at `[cr3]`. When the scheduler `archContextSwitch`es into the child it
  pops cr3/ebp/edi/esi/ebx and `ret`s into `ret_from_fork` with `esp` at `[ds]`.
- `ret_from_fork` is the existing ISR return tail (restore segs, `popad`, `add esp 8`,
  `iret`) — factored into a reusable label in `isr.S`. It `iret`s the copied frame →
  the child resumes in ring 3 right after its `fork()` with `eax=0`.

The trap frame copied is the live `Registers` the syscall handler received (the same
on-stack frame `popad`/`iret` will restore for the parent — so a faithful copy plus
`eax=0` makes the child return 0).

## Boot / init

PID 1 is created by the kernel (as in Stage 3, but via the new model): its body loads
`/disks/main/nanos/core/init.nxe` into its address space and `archEnterUser`s. `init`
itself `execve`s `/disks/main/nanos/bin/nsh.nxe` (PID 1 becomes the shell). When the
shell exits, PID 1 is gone and the kernel idles. The idle + clock kernel threads from
Stage 3 stay.

## nsh

Replace `spawn(path, argv)` with:
```c
int pid = fork();
if (pid == 0) { execve(path, argv); _exit(127); }   /* exec failed */
else { int st; waitpid(pid, &st, 0); }
```
`fork`/`execve`/`waitpid` wrappers go in the libc glue (`user/libc-glue/`). The bare-`ls`
and builtins (`exit`, `echo`) stay. `termmode` raw/cooked toggle around the child stays.

## Error handling

- `fork` out of process slots / frames → returns `-EAGAIN`.
- `execve` bad path → returns `-ENOENT` (the process keeps running its old image, so the
  shell's child does `_exit(127)`).
- `waitpid` with no children → `-ECHILD`.
- A process exiting while the parent is not waiting → stays `TASK_ZOMBIE` until reaped;
  if the parent exits first, the child is reparented to PID 1 (or reaped by the kernel).
  For Stage 4 (shell always waits), keep it simple: zombies are reaped by `waitpid`; an
  orphan whose parent died is reaped by the kernel.

## Testing

Host (MI, pure logic):
- `FdTable` copy (fork) — child sees the parent's open fds; closing in one doesn't
  affect the other after copy.
- waitpid/zombie state machine — exit sets ZOMBIE+code; waitpid finds a zombie child,
  returns pid+status, frees the slot; no-children → ECHILD.
- Address-space-free frame accounting over a fake `PagingEnv`: freeing a space releases
  exactly the user-window frames + user PT + dir, and not the kernel PTs.

QEMU (the asm trap-frame/ret_from_fork, CR3, teardown are hardware):
- a tiny `fork` test program: `fork()`, child prints "child", parent `waitpid`s and
  prints "parent got N" — both run, correct pids/status.
- nsh runs `ls`, `cat`, `echo` via fork/exec/wait; many commands in a row with no RAM
  exhaustion (teardown works); no `v=08/0d/0e` faults; `cat /proc/uptime` still advances.

## Files

- Create: `kernel/Process.h`/`.cpp` (process table, fd table, fork/exec/wait/exit MI
  glue), `tests/test_process.cpp`. `arch/x86/cpu/` gains `ret_from_fork` (in `isr.S`)
  and `archEnterUser`/`archForkChild` (in `usermode_x86.cpp` / a new `fork_x86.cpp`).
- Modify: `kernel/Syscall.{h,cpp}` (fd table → per-process; exit per-process),
  `kernel/SyscallDispatch.cpp` (fork/execve/waitpid; drop spawn), `kernel/SyscallNr.h`
  (numbers), `kernel/Scheduler.{h,cpp}` (ZOMBIE state, esp0 per task, set TSS.esp0 on
  switch), `arch/include/arch/{usermode,mmu}.h` (archEnterUser, mmuFreeAddressSpace),
  `arch/x86/cpu/usermode_x86.cpp` (drop enterUser/longjmp/spawn → archEnterUser),
  `arch/x86/mm/mmu_x86.cpp` (mmuFreeAddressSpace), `arch/x86/cpu/cpu_x86.cpp` (drop the
  depth kstacks; esp0 per-process), `kernel/Exec.cpp` (execProgram → exec into current
  process), `kernel/Kernel.cpp` (PID 1 via new model), `user/nsh.c`,
  `user/libc-glue/syscalls.c` (fork/execve/waitpid wrappers; drop spawn).
- Remove: the `spawnUserImage`/`g_depth`/`g_userCtx` paths; `SYS_spawn`.

## Build order

Steps 1–2 are additive and stay fully green. Steps 3–6 are the **cohesive process-model
switch**: removing synchronous `spawn` pulls out the `longjmp` foundation it stands on,
so `fork`/`execve`/`waitpid` must replace it as a unit. Through that window the kernel
boots and builtins work, but **`ls`/`cat` (which used `spawn`) are temporarily broken
until step 6** reconnects them via `fork`+`exec`+`wait`. Each step still builds + boots;
this is the unavoidable cost of the rearchitecture (one-big Stage 4, accepted).

1. **Per-process fd table + Process table** (no fork yet): the fd array moves into a
   `Process`/`FdTable` keyed by the running task; `Syscalls` reads the current process's
   fds. Single process unchanged — host tests + QEMU (`nsh`/`ls`/`cat`) still work.
2. **`mmuFreeAddressSpace`** (selective teardown) + host frame-accounting test. Additive,
   not called yet.
3. **Trap-frame model + `execve`** (one commit, so the system boots again): `archEnterUser`
   (no longjmp); `TSS.esp0 = current->esp0` on context switch; `exit` → free space +
   ZOMBIE + deschedule; drop `g_userCtx`/`spawnUserImage`/`g_depth`; `execve` (load into
   the current space, rewrite the trap frame). PID 1 starts via `archEnterUser`; `init.nxe`
   `execve`s `nsh`. QEMU: boots to `nsh$`; `echo`/`exit` work (`ls`/`cat` broken till step 6).
4. **`fork`** + `ret_from_fork` + child bootstrap. Verify with a tiny fork test program
   (printing from child + parent). QEMU.
5. **`waitpid`** + zombie reap; `exit` wakes a waiting parent. Verify the fork test
   program with the parent `waitpid`-ing the child.
6. **nsh → `fork`+`execve`+`waitpid`**; remove `SYS_spawn` + the spawn machinery; add the
   glue wrappers. QEMU: `ls`/`cat`/`echo` via fork/exec/wait, many commands with no RAM
   leak, no faults, `/proc/uptime` still advancing.
```
