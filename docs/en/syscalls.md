# NanOS Syscall Interface

NanOS exposes a **Linux i386 syscall ABI** over `int 0x80` — the same numbers and calling
convention real Linux i686 binaries use, which is what lets unmodified ports (ping, wget, bash,
vim) run (writing-apps.md, networking.md §4). The interface is a thin arch decode on
top of a host-testable, per-process `Syscalls` core over the VFS.

---

## 1. The ABI (`int 0x80`)

Calling convention (Linux i386): `eax` = number, args in `ebx, ecx, edx, esi, edi, ebp`, result in
`eax` — **negative = `-errno`**. The path:

```
int 0x80 ─► IDT gate 128 (DPL 3) ─► isr128 (isr.S) ─► isr_common_stub (pusha + load kernel ds)
  ─► syscallTrap (syscall_x86.cpp): sti; kernelSyscall(nr=eax, a0..a5=ebx.., tf) ─► result → regs.eax
  ─► (procExit if it exited) ─► signalDeliver(tf) ─► iret  (popa restores the modified eax)
```

- Gate 128 is installed **DPL 3** (`Idt.cpp`) so ring-3 code can issue it (boot.md §5).
- The trap frame is `kernel::Registers` on the task's kernel stack; `isr_handler` takes it by value,
  so writing `regs.eax` and `iret`/`popa` returns the value to the caller.
- The MI contract is `<arch/syscall.h>`: `kernelSyscall(nr, a0..a5, TrapFrame*)`, `syscallInit`,
  `syscallSelfTest`. Signal delivery and a possible `exit` happen at this return boundary
  (scheduler.md §6).

The userland side (`user/libc-glue/syscalls.c`) is just inline-asm wrappers (`sysN(nr, …)` →
`int $0x80`) plus `reterr()` turning a negative return into `errno` + `-1`. `kernel/SyscallNr.h` is
shared by kernel and userland (`-iquote kernel`).

---

## 2. The `Syscalls` core (`kernel/Syscall.*`)

A **host-testable** class: a per-process file-descriptor table over a `Vfs*` + an injected console
sink. `MAXFD = 128`; fds 0/1/2 are the console, `open` allocates ≥ 3. A fd uniformly backs a file
(path + offset), a pipe (`Pipe*`), a socket (`Socket*`), or a pty — so `read`/`write`/`close`/
`dup`/`poll` work the same on all. Each process owns a `Syscalls` instance (`Process::sys`), copied
on fork (fd table duplicated, pipe/socket refcounts bumped) and deleted on reap (scheduler.md
§5). Errors are negative errno (`ENOENT, EBADF, EINVAL, EROFS, EAGAIN, EPIPE, ENOTSOCK,
ECONNREFUSED, …`, defined in `Syscall.h`); unknown numbers return `-ENOSYS`.

`installSyscalls(vfs)` (`SyscallDispatch.cpp`, called at boot) inits the proc table, creates pid 1's
`Syscalls`, stores the global VFS (for `execve`), and calls `arch::syscallInit()`. The dispatch
routes to the current process via `ProcTable::current()->sys` (the Task→Process back-pointer).

---

## 3. The dispatch (`kernel/SyscallDispatch.cpp`)

`kernelSyscall` is the MI switch. The implemented set (Linux i386 numbers; ~100 entries):

**File I/O & metadata** — `read`(3) `write`(4) `open`(5) `close`(6) `creat`(8) `link`(9)
`unlink`(10) `chmod`(15) `lchown`(16) `lseek`(19) `utime`(30) `access`(33) `sync`(36) `rename`(38)
`mkdir`(39) `rmdir`(40) `umask`(60) `symlink`(83) `readlink`(85) `truncate`(92) `ftruncate`(93)
`fchmod`(94) `fchown`(95) `statfs`(99) `fstatfs`(100) `stat`(106) `lstat`(107) `fstat`(108)
`fsync`(118) `fchdir`(133) `fdatasync`(148) `chown`(182) `getdents64`(220) `utimes`(271)
`utimensat`(320).

**`*at` (dirfd-relative)** — `openat`(295) `mkdirat`(296) `fchownat`(298) `fstatat64`(300)
`unlinkat`(301) `renameat`(302) `linkat`(303) `symlinkat`(304) `readlinkat`(305) `fchmodat`(306)
`faccessat`(307) `renameat2`(353).

**Process** — `exit`(1) `fork`(2) `execve`(11) `chdir`(12) `getpid`(20) `waitpid`(7) `kill`(37)
`times`(43) `brk`(45) `getppid`(64) `reboot`(88) `getcwd`(183). (`exit_group` aliases `exit`.)

**Signals** — `pause`(29) `kill`(37) `signal`(48) `sigsuspend`(72) `sigreturn`(119)
`sigprocmask`(126) (+ `sigaction` family) — see scheduler.md §6.

**Job control / ids** — `setpgid`(57) `getpgrp`(65) `setsid`(66) `getpgid`(132) `getsid`(147);
credentials `getuid`(24/199) `setuid`(23/213) `getgid`(47/200) `setgid`(46/214) `geteuid`(49/201)
`getegid`(50/202) (NanOS is single-user root — these are largely identity stubs, see §4).

**Descriptors / pipes** — `pipe`(42) `dup`(41) `dup2`(63) `fcntl`(55) `ioctl`(54).

**Polling** — `poll`(168) `_newselect`(142) (the 5-arg `select` real glibc/uClibc emit).

**Sockets** — `socketcall`(102) (the multiplexed entry: `SC_SOCKET … SC_RECVMSG` 1–18 — what
inetutils `ping`/`wget` use) **and** the direct calls `socket`(359) `socketpair`(360) `bind`(361)
`connect`(362) `listen`(363) `accept4`(364) `getsockopt`(365) `setsockopt`(366) `getsockname`(367)
`getpeername`(368) `sendto`(369) `sendmsg`(370) `recvfrom`(371) `recvmsg`(372) `shutdown`(373) —
see networking.md §4.

**Memory** — `brk`(45) `mmap2`(192) (anonymous + file-backed + the framebuffer, memory.md §6).

**Time / random** — `nanosleep`(162) `clock_gettime`(265) `getrandom`(355).

**NanOS-private** — `termmode`(501): switch console input between cooked and raw (terminal.md).

Blocking syscalls are signal-interruptible (`-ERESTARTSYS` → restart or `-EINTR`, per the handler's
`SA_RESTART`).

---

## 4. Stubs (not real syscalls)

`user/libc-glue/posixstubs.c` fakes what a single-user, read-only-root system doesn't need:
the uid/gid identity calls return 0 (root); `chown`/`chmod` on the read-only root are no-ops;
`getrusage` zeros; `getrlimit`/`setrlimit` report unlimited; `sigaltstack`/`alarm` are no-ops;
`chroot` is `-ENOSYS`; `uname`/`pathconf` return fixed NanOS values. (Things like `getpwuid` are
*not* stubs — they read `/etc/passwd` through real syscalls.)

---

## 5. Validation

The socket ABI was checked against real `strace` captures (`tests/fixtures/strace/`:
inetutils-ping, wget-http) so the numbers, `socketcall` subcalls, sockopts and blocking semantics
match what Linux binaries expect. The `Syscalls` core itself is in the host doctest + coverage gate.

**Key files:** `arch/x86/cpu/{isr.S, syscall_x86.cpp, Idt.cpp}`, `arch/include/arch/syscall.h`,
`kernel/{SyscallNr.h, SyscallDispatch.cpp, Syscall.*}`, `user/libc-glue/syscalls.c`.
