# Interfejs syscalli NanOS

NanOS udostępnia **Linux i386 syscall ABI** poprzez `int 0x80` — te same numery i konwencję
wywołań, których używają prawdziwe binarki Linux i686, co właśnie pozwala uruchamiać niezmodyfikowane
porty (ping, wget, bash, vim) (writing-apps.md, networking.md §4). Interfejs to cienkie dekodowanie
po stronie arch na szczycie hostowalnego do testów, per-proces rdzenia `Syscalls` nad VFS.

---

## 1. ABI (`int 0x80`)

Konwencja wywołań (Linux i386): `eax` = numer, argumenty w `ebx, ecx, edx, esi, edi, ebp`, wynik w
`eax` — **wartość ujemna = `-errno`**. Ścieżka:

```
int 0x80 ─► IDT gate 128 (DPL 3) ─► isr128 (isr.S) ─► isr_common_stub (pusha + load kernel ds)
  ─► syscallTrap (syscall_x86.cpp): sti; kernelSyscall(nr=eax, a0..a5=ebx.., tf) ─► result → regs.eax
  ─► (procExit if it exited) ─► signalDeliver(tf) ─► iret  (popa restores the modified eax)
```

- Brama 128 jest zainstalowana jako **DPL 3** (`Idt.cpp`), aby kod ring-3 mógł ją wywoływać (boot.md §5).
- Trap frame to `kernel::Registers` na stosie kernelowym zadania; `isr_handler` przyjmuje go przez
  wartość, więc zapisanie `regs.eax` oraz `iret`/`popa` zwraca wartość do wywołującego.
- Kontraktem MI jest `<arch/syscall.h>`: `kernelSyscall(nr, a0..a5, TrapFrame*)`, `syscallInit`,
  `syscallSelfTest`. Dostarczanie sygnałów i ewentualny `exit` dzieją się na tej granicy powrotu
  (scheduler.md §6).

Strona userlandu (`user/libc-glue/syscalls.c`) to po prostu wrappery z inline-asm (`sysN(nr, …)` →
`int $0x80`) plus `reterr()` zamieniający ujemny wynik na `errno` + `-1`. `kernel/SyscallNr.h` jest
współdzielony przez kernel i userland (`-iquote kernel`).

---

## 2. Rdzeń `Syscalls` (`kernel/Syscall.*`)

Klasa **hostowalna do testów**: per-proces tablica deskryptorów plików nad `Vfs*` + wstrzyknięty
sink konsoli. `MAXFD = 128`; fd 0/1/2 to konsola, `open` alokuje ≥ 3. Fd jednolicie reprezentuje plik
(ścieżka + offset), pipe (`Pipe*`), gniazdo (`Socket*`) lub pty — więc `read`/`write`/`close`/
`dup`/`poll` działają tak samo na wszystkich. Każdy proces posiada instancję `Syscalls` (`Process::sys`),
kopiowaną przy fork (tablica fd duplikowana, refcounty pipe/socket zwiększane) i usuwaną przy reap
(scheduler.md §5). Błędy to ujemne errno (`ENOENT, EBADF, EINVAL, EROFS, EAGAIN, EPIPE, ENOTSOCK,
ECONNREFUSED, …`, zdefiniowane w `Syscall.h`); nieznane numery zwracają `-ENOSYS`.

`installSyscalls(vfs)` (`SyscallDispatch.cpp`, wołane przy starcie) inicjuje tablicę procesów, tworzy
`Syscalls` dla pid 1, zapamiętuje globalny VFS (dla `execve`) i wywołuje `arch::syscallInit()`. Dispatch
kieruje do bieżącego procesu przez `ProcTable::current()->sys` (wskaźnik wsteczny Task→Process).

---

## 3. Dispatch (`kernel/SyscallDispatch.cpp`)

`kernelSyscall` to switch MI. Zaimplementowany zbiór (numery Linux i386; ~100 wpisów):

**File I/O & metadane** — `read`(3) `write`(4) `open`(5) `close`(6) `creat`(8) `link`(9)
`unlink`(10) `chmod`(15) `lchown`(16) `lseek`(19) `utime`(30) `access`(33) `sync`(36) `rename`(38)
`mkdir`(39) `rmdir`(40) `umask`(60) `symlink`(83) `readlink`(85) `truncate`(92) `ftruncate`(93)
`fchmod`(94) `fchown`(95) `statfs`(99) `fstatfs`(100) `stat`(106) `lstat`(107) `fstat`(108)
`fsync`(118) `fchdir`(133) `fdatasync`(148) `chown`(182) `getdents64`(220) `utimes`(271)
`utimensat`(320).

**`*at` (względne wobec dirfd)** — `openat`(295) `mkdirat`(296) `fchownat`(298) `fstatat64`(300)
`unlinkat`(301) `renameat`(302) `linkat`(303) `symlinkat`(304) `readlinkat`(305) `fchmodat`(306)
`faccessat`(307) `renameat2`(353).

**Proces** — `exit`(1) `fork`(2) `execve`(11) `chdir`(12) `getpid`(20) `waitpid`(7) `kill`(37)
`times`(43) `brk`(45) `getppid`(64) `reboot`(88) `getcwd`(183). (`exit_group` to alias `exit`.)

**Sygnały** — `pause`(29) `kill`(37) `signal`(48) `sigsuspend`(72) `sigreturn`(119)
`sigprocmask`(126) (+ rodzina `sigaction`) — patrz scheduler.md §6.

**Kontrola zadań / identyfikatory** — `setpgid`(57) `getpgrp`(65) `setsid`(66) `getpgid`(132) `getsid`(147);
poświadczenia `getuid`(24/199) `setuid`(23/213) `getgid`(47/200) `setgid`(46/214) `geteuid`(49/201)
`getegid`(50/202) (NanOS jest jednoużytkownikowy jako root — to w większości zaślepki tożsamości, patrz §4).

**Deskryptory / pipe** — `pipe`(42) `dup`(41) `dup2`(63) `fcntl`(55) `ioctl`(54).

**Polling** — `poll`(168) `_newselect`(142) (5-argumentowy `select`, który emituje prawdziwe glibc/uClibc).

**Gniazda** — `socketcall`(102) (zmultipleksowane wejście: `SC_SOCKET … SC_RECVMSG` 1–18 — to, czego
używają `ping`/`wget` z inetutils) **oraz** wywołania bezpośrednie `socket`(359) `socketpair`(360) `bind`(361)
`connect`(362) `listen`(363) `accept4`(364) `getsockopt`(365) `setsockopt`(366) `getsockname`(367)
`getpeername`(368) `sendto`(369) `sendmsg`(370) `recvfrom`(371) `recvmsg`(372) `shutdown`(373) —
patrz networking.md §4.

**Pamięć** — `brk`(45) `mmap2`(192) (anonimowa + plikowa + framebuffer, memory.md §6).

**Czas / losowość** — `nanosleep`(162) `clock_gettime`(265) `getrandom`(355).

**Prywatne dla NanOS** — `termmode`(501): przełącza wejście konsoli między trybem cooked a raw (terminal.md).

Blokujące syscalle są przerywalne sygnałem (`-ERESTARTSYS` → restart lub `-EINTR`, zależnie od
`SA_RESTART` handlera).

---

## 4. Zaślepki (nie prawdziwe syscalle)

`user/libc-glue/posixstubs.c` udaje to, czego jednoużytkownikowy system z read-only rootem nie potrzebuje:
wywołania tożsamości uid/gid zwracają 0 (root); `chown`/`chmod` na read-only roocie to no-opy;
`getrusage` zeruje; `getrlimit`/`setrlimit` raportują brak limitów; `sigaltstack`/`alarm` to no-opy;
`chroot` to `-ENOSYS`; `uname`/`pathconf` zwracają stałe wartości NanOS. (Rzeczy takie jak `getpwuid`
*nie* są zaślepkami — czytają `/etc/passwd` przez prawdziwe syscalle.)

---

## 5. Walidacja

Socket ABI zostało sprawdzone wobec prawdziwych przechwytów `strace` (`tests/fixtures/strace/`:
inetutils-ping, wget-http), aby numery, podwywołania `socketcall`, sockopty i semantyka blokowania
odpowiadały temu, czego oczekują binarki Linuksa. Sam rdzeń `Syscalls` znajduje się w hostowym doctest
+ bramce pokrycia.

**Key files:** `arch/x86/cpu/{isr.S, syscall_x86.cpp, Idt.cpp}`, `arch/include/arch/syscall.h`,
`kernel/{SyscallNr.h, SyscallDispatch.cpp, Syscall.*}`, `user/libc-glue/syscalls.c`.
