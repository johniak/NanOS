# x86_64 Plan 6 — Syscalle (syscall/sysret) + userland + format .nxe v4

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Doprowadzić ring-3 userland na x86_64. Po tym planie 64-bitowy `init.nxe`
(zbudowany kernelowym toolchainem `x86_64-elf`, ze źródeł w `user/`) **ładuje się,
wiąże swój IAT, biegnie w ringu 3**, wykonuje syscalle instrukcją `syscall`, czyta
`grub.cfg` i kończy `exit()` — wracając do kernela bez faultu. To pierwszy 64-bitowy
program użytkownika. (Pełny picolibc/`libc.ndl` + `nsh` to Plan 8 — tu dowodzimy
ŚCIEŻKI: `syscall`/`sysret`, `%fs.base`, format `.nxe` v4, `mknx`/`NxeLoader` 64-bit,
crt0 SysV AMD64, `iretq` do ringu 3.)

**Architecture:** Migracja-zastąpienie (spec §0.1). MD piszemy w `arch/x86_64/cpu/`
(`syscall_x86_64.cpp`, `usermode_x86_64.cpp`, `nxjmp64.S`, `sigtramp` userland), MI
rdzeń (`kernel/Syscall.cpp` `Syscalls`, `SyscallDispatch.cpp` `kernelSyscall`,
`NxeLoader`) jest host-testowany i poszerzany na LP64 w MINIMALNYM zakresie potrzebnym
tutaj (reszta sweepu = Plan 7). Format `.nxe`/`mknx`/`NxeLoader` rozwijamy **TDD na
hoście** (mają testy doctest). i686 pozostaje zielony przez cały plan dzięki
warunkowemu (`#if defined(__x86_64__)`) typowaniu w nagłówkach współdzielonych — Plan 1
ustalił filozofię „oba zielone w okresie przejściowym".

**Tech Stack:** `x86_64-elf-gcc`/`ld`/NASM (`-f elf64`) z Planu 1; `qemu-system-x86_64`;
Docker `nanos-build`; host `cc` + doctest dla `mknx`/`NxeLoader`. Wzorce: AMD64 SysV ABI
(numery syscalli x86_64, argumenty `rdi/rsi/rdx/r10/r8/r9`), kanoniczny OSDev
„SYSCALL/SYSRET" (MSR `EFER.SCE`, `STAR`/`LSTAR`/`FMASK`, `swapgs`).

**Reference:** spec §5.1 (ABI syscalli), §5.2 (.nxe/.ndl v4), §5.4 decyzje #1 (model
kodu/relokacje), #2 (`%fs.base`), #3 (SSE), §2.2 (`syscall_x86`/`usermode_x86`/`nxjmp`),
§4 pkt 3/7/8/9 (LP64: args dispatchu, `Exec`, `Fd`/`lseek`, `struct stat`). Trzy decyzje
§5.4 są SEDNEM tego planu — rozwinięte w Tasku 0 (poniżej).

### Decyzje projektowe wmontowane w plan (spec §5.4)

- **#1 — model kodu small, non-PIC, niska baza (jedyna nie-mechaniczna decyzja).** Kod w
  modelu *small* na x86_64 jest **RIP-relative** → relokacje `R_X86_64_PC32`, których
  loader NIE rusza (pozycyjnie niezależne). **Wskaźnikowe DANE** (gniazda IAT, tablice
  wskaźników, zainicjalizowane pointery) emitują `R_X86_64_64` (8-bajtowe absolutne) —
  `mknx` MUSI je zebrać, a loader robi `*(uint64_t*)(img+off) += delta64`. Część
  konstrukcji small non-PIC emituje `R_X86_64_32S` (32-bit, sign-extended) — te
  **wymuszają obraz w dolnych 2 GiB**. **Decyzja:** trzymamy bazy ładowania user w
  dolnych 2 GiB (baza `.nxe` = `0x800000`, jak i686 — niska, kanoniczna) + model small,
  uczymy `mknx` relokacji `R_X86_64_64` (+ akceptacji `R_X86_64_32S` jako „nie-reloc,
  ale wymóg low-2GiB"), poszerzamy `NxReloc.off` do 64-bit. Build user dostaje
  `-mcmodel=small -fno-pic`.
- **#2 — `%fs.base` per proces.** picolibc x86_64 trzyma wskaźnik wątku (errno, stdio) w
  `%fs.base`. Bez ustawienia go PIERWSZY dostęp do TLS-owego `errno` faultuje. Kernel
  ustawia `IA32_FS_BASE` (MSR `0xC0000100`) z `thread->tlsBase` przy wejściu w ring 3 i
  przy każdym przełączeniu kontekstu na zadanie ringu-3. To ścieżka NIEOBECNA na i686
  (tam TLS to wpis GDT `%gs`).
- **#3 — asymetria SSE.** ABI SysV AMD64 *wymaga* SSE (float/varargs w XMM). **Userland
  MUSI mieć SSE** (zdejmujemy `-mno-sse` dla `user/`/picolibc na x86_64). **Kernel NIE
  MOŻE** (`-mno-sse -mno-mmx -mno-red-zone -mgeneral-regs-only`, już ustawione w
  `arch/x86_64/arch.mk` `KARCHFLAGS` z Planu 1). Oba zestawy flag jawnie odseparowane.

---

## File Structure

| Plik | Odpowiedzialność | Akcja |
|---|---|---|
| `kernel/SyscallNr.h` | numery syscalli — warunkowo x86_64 (`read=0,write=1,…,exit=60,mmap=9`) vs i386; współdzielone z SDK | Modify |
| `arch/include/arch/syscall.h` | poszerzyć `kernelSyscall` args `unsigned`→`unsigned long` (per-arch szerokość) | Modify |
| `kernel/SyscallDispatch.{h,cpp}` | sygnatura `kernelSyscall` + lokalne `unsigned A[6]`→`unsigned long A[6]` (minimalny LP64; reszta Plan 7) | Modify |
| `arch/include/arch/usermode.h` | poszerzyć adresowe parametry `uint32_t`→`uint64_t` (per-arch przez `uintptr_t`) | Modify |
| `arch/x86_64/cpu/syscall_x86_64.cpp` | implementuje `<arch/syscall.h>`: init MSR `EFER.SCE`/`STAR`/`LSTAR`/`FMASK`, stub `syscallEntry` (swapgs+kstack+ramka), dekodowanie `rdi…r9`→`kernelSyscall` | Create |
| `arch/x86_64/cpu/syscall_entry64.S` | NASM elf64: punkt wejścia `syscall_entry` (zrzut GPR, `swapgs`, `sysretq`) | Create |
| `arch/x86_64/cpu/usermode_x86_64.cpp` | implementuje `<arch/usermode.h>`: `archLoadUser` (stos SysV 8-bajtowy), `archEnterUser` (`iretq` do ring3 + `%fs.base`), `archFrameToUser`, ramka sygnału 64-bit, `archSigreturn` | Create |
| `arch/x86_64/cpu/nxjmp64.S` | NASM elf64: `nx_setjmp`/`nx_longjmp` (rsp/rbp/rbx/r12–r15/rip) dla `exit()` | Create |
| `kernel/NxFormat.h` | format v4: pola adresowe `nxaddr_t` (=`uint64_t` na x86_64), `NX_VERSION 4` | Modify |
| `kernel/NxeLoader.{h,cpp}` | zapisy 64-bit (`*(nxaddr_t*)`), odrzucenie wersji ≠ bieżącej, `entryOut`/`loadDelta` na `nxaddr_t` | Modify |
| `kernel/Exec.cpp` | `unsigned entry`→`nxaddr_t entry` (minimalny LP64 styk z loaderem) | Modify |
| `tools/mknx.c` | ELF64 (`Elf64_*`, `EM_X86_64`, `R_X86_64_64/PC32/32S`, `ELF64_*`); pod `-DNX_FORCE64` emituje v4 | Modify |
| `tests/test_nxeloader.cpp` | przepisać fixture'y na v4 (`NX_FORCE64`, `nxaddr_t`, 8-bajtowe sloty) | Modify |
| `user/crt064.S` | NASM elf64: `_start` SysV AMD64 (argc/argv/envp z `rsp`, args do `main` w `rdi/rsi/rdx`, 16-align) | Create |
| `user/sigtramp64.S` | NASM elf64: `__nx_sigtramp` x86_64 (`mov rax, SYS_rt_sigreturn; syscall`) | Create |
| `user/libnanos.c` | warianty `syscall` (klobry `rcx,r11`) pod `#ifdef __x86_64__`; typy `long` | Modify |
| `user/nx.ld` | warianty 64-bit przez `arch/x86_64/user-nx.ld` (baza `0x800000`, `OUTPUT_FORMAT(elf64-x86-64)`) | Create |
| `Makefile` | x86_64 userland recipe (`UARCHFLAGS`, `mknx64`, `-f elf64` dla user .S), cel `init64`/`_userland64` | Modify |
| `arch/x86_64/arch.mk` | `ARCH_SOURCES` += `syscall_x86_64.o syscall_entry64.o usermode_x86_64.o nxjmp64.o` | Modify |

---

## Task 0: Kontrakt nazewniczy + założenia o Planach 2–5 (przeczytaj, nie koduj)

- [ ] **Step 1: Potwierdzić zależności od wcześniejszych planów**

Ten plan zakłada DOSTARCZONE przez Plany 2–5 (patrz `## Zależności`):
- **Plan 2:** MI `kmain`/`Kernel::start` biegnie na x86_64; `Console` działa.
- **Plan 3:** paging 4-poziomowy; `arch::AddressSpace`, `mmuMap`, `mmuSwitch`,
  `mmuCreateAddressSpace`, `mmuUserHeapBase/Max`, `mmuCurrentDirPhys`, `g_frames.alloc()`.
- **Plan 4:** **GDT/IDT/TSS 64-bit + `kernel::Registers` (x86_64 TrapFrame)**, `iretq`,
  `swapgs`, `arch::setKernelStack(uint64_t rsp0)`. **Założony układ selektorów GDT (musi
  go spełnić Plan 4, bo `SYSRET` go wymusza):**
  ```
  0x08  kernel code64 (L=1, DPL0)        SYSCALL ładuje CS=STAR[47:32]
  0x10  kernel data   (DPL0)             SYSCALL ładuje SS=STAR[47:32]+8
  0x18  user code32 / placeholder (DPL3) SYSRET liczy od STAR[63:48]
  0x20  user data     (DPL3)             SYSRET: SS=STAR[63:48]+8  -> 0x23
  0x28  user code64   (L=1, DPL3)        SYSRET: CS=STAR[63:48]+16 -> 0x2B
  ```
  → selektory ringu 3: **CS=`0x2B`, SS/DS=`0x23`**. `STAR` = `(0x08<<32) | (0x18<<48)`.
  Jeśli Plan 4 ułożył GDT inaczej, ZSYNCHRONIZUJ stałe w `syscall_x86_64.cpp` +
  `usermode_x86_64.cpp` i odnotuj w Self-Review.
- **Plan 5:** boot z ext4; `/disks/main` zamontowane (do odczytu `grub.cfg`).

- [ ] **Step 2: Zanotować nakładkę z Planem 7 (LP64).** Tutaj poszerzamy TYLKO:
  `kernelSyscall` args (`unsigned`→`unsigned long`), `Exec.cpp` `entry`, `NxFormat`/
  `NxeLoader` adresy, `usermode.h` adresy. Reszta (`FrameAllocator`, `Console::writeHex`,
  `Fd.offset`/`lseek` na `off_t`, `LinuxStat.st_size` 64-bit) NALEŻY do Planu 7 — nie
  ruszamy jej tu poza wymuszonym `struct stat` ABI (Task 12, niżej). Nie ma kroku kodu.

---

## Task 1: Numery syscalli x86_64 (warunkowo w SyscallNr.h)

**Files:** Modify `kernel/SyscallNr.h`

- [ ] **Step 1: Owinąć numery w `#if defined(__x86_64__)`**

`SyscallNr.h` jest współdzielony przez kernel i userland — oba kompilowane właściwym
toolchainem, więc `__x86_64__` rozróżnia ABI bez osobnego pliku. i686 zostaje nietknięty
(gałąź `#else`). Zamień blok `#define SYS_*` (linie 11–172) na:

```c
#if defined(__x86_64__)
/* ---- Linux x86_64 ABI (arch/x86/entry/syscalls/syscall_64.tbl) ---------------------- */
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lstat 6
#define SYS_poll 7
#define SYS_lseek 8
#define SYS_mmap 9          /* x86_64 has ONE mmap (no mmap2); off is in BYTES */
#define SYS_mmap2 SYS_mmap  /* libc-glue calls SYS_mmap2 by name — alias to the real number */
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn 15
#define SYS_sigreturn SYS_rt_sigreturn   /* x86_64 has only rt_sigreturn */
#define SYS_ioctl 16
#define SYS_pread64 17
#define SYS_pwrite64 18
#define SYS_access 21
#define SYS_pipe 22
#define SYS__newselect 23   /* x86_64 select */
#define SYS_select 23
#define SYS_dup 32
#define SYS_dup2 33
#define SYS_pause 34
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_socket 41
#define SYS_connect 42
#define SYS_accept4 288     /* x86_64: accept=43, accept4=288 (we use accept4) */
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_sendmsg 46
#define SYS_recvmsg 47
#define SYS_shutdown 48
#define SYS_bind 49
#define SYS_listen 50
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_socketpair 53
#define SYS_setsockopt 54
#define SYS_getsockopt 55
#define SYS_clone 56
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_waitpid 61      /* x86_64 has wait4=61; our dispatch treats it as waitpid */
#define SYS_kill 62
#define SYS_fcntl 72
#define SYS_fsync 74
#define SYS_fdatasync 75
#define SYS_truncate 76
#define SYS_ftruncate 77
#define SYS_getdents64 217
#define SYS_chdir 80
#define SYS_fchdir 81
#define SYS_rename 82
#define SYS_mkdir 83
#define SYS_rmdir 84
#define SYS_creat 85
#define SYS_link 86
#define SYS_unlink 87
#define SYS_symlink 88
#define SYS_readlink 89
#define SYS_chmod 90
#define SYS_fchmod 91
#define SYS_chown 92
#define SYS_fchown 93
#define SYS_lchown 94
#define SYS_umask 95
#define SYS_gettimeofday 96
#define SYS_getppid 110
#define SYS_setsid 112
#define SYS_setpgid 109
#define SYS_getpgrp 111
#define SYS_getpgid 121
#define SYS_getsid 124
#define SYS_getuid 102
#define SYS_getgid 104
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_sigprocmask SYS_rt_sigprocmask
#define SYS_rt_sigpending 127
#define SYS_rt_sigsuspend 130
#define SYS_sigsuspend SYS_rt_sigsuspend
#define SYS_utime 132
#define SYS_statfs 137
#define SYS_fstatfs 138
#define SYS_setsid 112
#define SYS_gettid 186
#define SYS_futex 202
#define SYS_set_thread_area 205   /* unused on x86_64 (TLS via arch_prctl); kept for the dispatch */
#define SYS_getrandom 318
#define SYS_clock_gettime 228
#define SYS_set_tid_address 218
#define SYS_tkill 200
#define SYS_tgkill 234
#define SYS_exit_group 231
#define SYS_utimes 235
#define SYS_utimensat 280
#define SYS_openat 257
#define SYS_mkdirat 258
#define SYS_unlinkat 263
#define SYS_renameat 264
#define SYS_renameat2 316
#define SYS_linkat 265
#define SYS_symlinkat 266
#define SYS_readlinkat 267
#define SYS_fchmodat 268
#define SYS_fchownat 260
#define SYS_faccessat 269
#define SYS_fstatat64 262    /* x86_64 newfstatat */
#define SYS_sync 162
#define SYS_reboot 169
#define SYS_arch_prctl 158   /* NEW on x86_64: TLS base (ARCH_SET_FS) — see %fs.base path */
#define SYS_pselect6 270
/* NanOS-private (outside the Linux range). */
#define SYS_termmode 1000
#else
/* ---- existing Linux i386 ABI (unchanged) ---------------------------------------------
 * ... the current #define block stays verbatim here ... */
#define SYS_exit 1
/* (… all existing i386 numbers, verbatim from the current file …) */
#define SYS_termmode 501
#endif
```

> UWAGA: zachowaj cały istniejący i386 blok w gałęzi `#else` BEZ ZMIAN (wklejony tu
> skrótowo). Aliasy (`SYS_mmap2→SYS_mmap`, `SYS_sigreturn→SYS_rt_sigreturn`,
> `SYS_select→23`) pozwalają, by `SyscallDispatch.cpp` używał tych samych etykiet `case`
> na obu architekturach bez `#ifdef` w switchu. `struct k_sigaction` (po blokach) zostaje
> wspólny — ale na x86_64 jego pola wskaźnikowe są 8-bajtowe automatycznie (`void*`).

- [ ] **Step 2: Kompilacyjna weryfikacja obu ABI**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build sh -c '
  echo "#include \"SyscallNr.h\"\nint w=SYS_write,e=SYS_exit;" > /tmp/t.c &&
  i686-elf-gcc   -Ikernel -c /tmp/t.c -o /tmp/t32.o &&
  x86_64-elf-gcc -Ikernel -c /tmp/t.c -o /tmp/t64.o &&
  echo OK'
```
Expected: `OK` (oba toolchainy kompilują nagłówek; `SYS_write`=4 na i686, =1 na x86_64).

- [ ] **Step 3: Commit**

```bash
git add kernel/SyscallNr.h
git commit -m "syscall: add x86_64 syscall numbers (conditional on __x86_64__), keep i386"
```

---

## Task 2: Poszerzenie sygnatury kernelSyscall na unsigned long (minimalny LP64)

**Files:** Modify `arch/include/arch/syscall.h`, `kernel/SyscallDispatch.{h,cpp}`

- [ ] **Step 1: `arch/syscall.h` — argumenty `unsigned`→`unsigned long`**

`unsigned long` to 32-bit na i686-elf (ILP32) i 64-bit na x86_64-elf (LP64) — dokładnie
żądana szerokość per arch, bez `#ifdef`. Na x86_64 to KONIECZNE: argumenty syscalli niosą
64-bitowe wskaźniki/rozmiary, które `unsigned` (32-bit) by obciął. Zamień deklarację:

```cpp
namespace kernel {
// MI dispatch: map a syscall number + up to 6 args to a result (negative errno on
// failure). Args are unsigned long so a 64-bit pointer/size survives on LP64 (x86_64);
// on i386 unsigned long is 32-bit, identical to before. `tf` is the opaque trap frame the
// arch returns through. Defined in kernel/SyscallDispatch.cpp.
int kernelSyscall(int nr, unsigned long a0, unsigned long a1, unsigned long a2,
		unsigned long a3, unsigned long a4, unsigned long a5, arch::TrapFrame* tf);
}
```

- [ ] **Step 2: `SyscallDispatch.cpp` — dopasować definicję + lokalne tablice arg**

Zmień nagłówek definicji (l. 376) na `unsigned long a0…a5`, oraz każde
`unsigned A[6]` w `socketOp`/dispatchu (l. 569 i 575–589) na `unsigned long A[6]`.
`socketOp(Syscalls*, int, const unsigned*)` → `const unsigned long*`; w jego ciele
rzutowania `(int) A[0]`/`(const char*) A[1]` zostają (na x86_64 `(const char*)` z
`unsigned long` jest pełne 64-bit). `iovGather`/`iovScatter` biorą `const unsigned*` →
`const unsigned long*` (iovec base/len są wskaźnikiem/rozmiarem — na x86_64 8-bajtowe).

```cpp
int kernelSyscall(int nr, unsigned long a0, unsigned long a1, unsigned long a2,
		unsigned long a3, unsigned long a4, unsigned long a5, arch::TrapFrame* tf) {
	int ret = -38;   // -ENOSYS
	Syscalls* g_sys = ProcTable::current()->sys;
	switch (nr) {
	/* ... ciało bez zmian poza A[6] -> unsigned long A[6] i (int)/(void*) rzutami ... */
```

> To jedyna LP64-zmiana w dispatchu w tym planie. Pełny audyt `-Wconversion` (przepełnienia
> w środku, `LinuxStat`, `off_t`) — Plan 7. Tu wystarczy, by 64-bitowe wskaźniki
> użytkownika DOTARŁY nieobcięte do `Syscalls`.

- [ ] **Step 3: Host-test bez regresji + i686 build**

```bash
make test                                   # MI dispatch/Syscalls host-tests still green
docker run --rm -v $(pwd):/src -w /src nanos-build make build   # i686 kernel still builds
```
Expected: testy zielone (na hoście `unsigned long` = 64-bit, sygnatura spójna); i686
kernel się buduje (`unsigned long` = 32-bit, zachowanie bez zmian).

- [ ] **Step 4: Commit**

```bash
git add arch/include/arch/syscall.h kernel/SyscallDispatch.h kernel/SyscallDispatch.cpp
git commit -m "syscall: widen kernelSyscall args to unsigned long (LP64-correct on x86_64)"
```

---

## Task 3: Format .nxe v4 — NxFormat.h (TDD na hoście zaczyna się tu)

**Files:** Modify `kernel/NxFormat.h`

- [ ] **Step 1: Wprowadzić `nxaddr_t` + wersję 4 warunkowo**

Na x86_64 (i pod `-DNX_FORCE64` dla `mknx64`/host-testów) pola adresowe stają się
8-bajtowe i wersja rośnie do 4; i686 zachowuje 4-bajtowe pola i wersję 3. To „czyste
cięcie" (loader x86_64 nie czyta v3 w runtime), a jednocześnie i686 buduje się i działa
przez cały okres przejściowy. Zamień górę pliku:

```c
#ifndef NXFORMAT_H
#define NXFORMAT_H
#include <stdint.h>

#define NX_MAGIC   0x0045584E   /* 'N','X','E',0 little-endian */
#define NX_FLAG_DLL 1u

#if defined(__x86_64__) || defined(NX_FORCE64)
#define NX_VERSION 4
typedef uint64_t nxaddr_t;      /* absolute module addresses are 64-bit on x86_64 */
#else
#define NX_VERSION 3
typedef uint32_t nxaddr_t;      /* i386: 32-bit, identical layout to the historical format */
#endif
```

Zamień KAŻDE `unsigned` w polach struktur `NxImport`/`NxExport`/`NxReloc`/`NxNeeded`/
`NxHeader` na `nxaddr_t`, oraz `magic`/`version`/`flags`/`importCount`/… (liczniki) na
`uint32_t` (liczniki nie są adresami i mają stałą szerokość po obu stronach):

```c
typedef struct { nxaddr_t nameOff; nxaddr_t slotAddr; nxaddr_t libOff; } NxImport;
typedef struct { nxaddr_t nameOff; nxaddr_t addr; } NxExport;
typedef struct { nxaddr_t off; } NxReloc;
typedef struct { nxaddr_t nameOff; } NxNeeded;

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t flags;
	uint32_t _pad;            /* keep entry 8-aligned on x86_64 */
	nxaddr_t entry;
	nxaddr_t loadBase;
	nxaddr_t imageSize;
	nxaddr_t bssStart;
	nxaddr_t bssEnd;
	nxaddr_t importTable;  uint32_t importCount;  uint32_t _p0;
	nxaddr_t exportTable;  uint32_t exportCount;  uint32_t _p1;
	nxaddr_t relocTable;   uint32_t relocCount;   uint32_t _p2;
	nxaddr_t neededTable;  uint32_t neededCount;  uint32_t _p3;
} NxHeader;

#endif /* NXFORMAT_H */
```

> Padding (`_pad`/`_pN`) trzyma 8-bajtowe pola wyrównane na x86_64 i daje deterministyczny
> layout, który `mknx` i loader muszą wypełniać identycznie. Na i686 `nxaddr_t`=`uint32_t`,
> więc layout różni się od historycznego v3 (doszły `_pad`/`_pN`) — to OK, bo i686 `mknx`
> i loader są przekompilowywane z tego samego nagłówka (zgodność wewnątrz buildu, nie z
> binariami na dysku — które i tak przebudowujemy).

- [ ] **Step 2: Commit (sam nagłówek — testy w Tasku 4)**

```bash
git add kernel/NxFormat.h
git commit -m "nxe: format v4 — 64-bit address fields (nxaddr_t), version 4 on x86_64"
```

---

## Task 4: NxeLoader 64-bit — TDD na hoście

**Files:** Modify `tests/test_nxeloader.cpp` (najpierw), `kernel/NxeLoader.{h,cpp}`

- [ ] **Step 1: Przepisać testy na v4 (NAJPIERW — mają zfailować)**

Na górze `tests/test_nxeloader.cpp` wymuś v4 PRZED dołączeniem nagłówka (host jest arm64,
więc `__x86_64__` jest niezdefiniowany — `NX_FORCE64` włącza ścieżkę 64-bit do testów):

```cpp
#define NX_FORCE64 1
#include "doctest.h"
#include "NxeLoader.h"
#include <cstring>
#include <cstdint>
using namespace kernel;
```

Zamień typy w fixture'ach na 64-bit: `entry` → `nxaddr_t` (`uint64_t`), sloty IAT
sprawdzane jako `*(void**)` (8 bajtów), relokowane słowa jako `*(uint64_t*)`. Baza
testowa zostaje niska (`0x400000`). Przykład pierwszego testu:

```cpp
static const nxaddr_t BASE = 0x400000;
static nxaddr_t A(nxaddr_t off) { return BASE + off; }

TEST_CASE("NxeLoader binds imports, zeroes bss, returns entry (v4)") {
	char buf[512];
	buildImage(buf);                 // h->version = NX_VERSION (==4 under NX_FORCE64)
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == 0);
	CHECK(entry == A(0x40));
	CHECK(*(void**) (buf + 0x180) == (void*) 0x11110000);   // 8-byte IAT slot
	CHECK(buf[0x100] == 0);                                  // bss zeroed
}
```

W `buildImage`/`buildRelocImage` ustaw `h->version = NX_VERSION;`, a sloty importów
rozsuń na 8 bajtów (`slotAddr = A(0x180)` i `A(0x190)` — nie `0x188`, bo slot jest teraz
8-bajtowy). Relokowane słowa zapisuj `*(uint64_t*)(buf+0x40)=A(0x40);`. Dodaj NOWY test:

```cpp
TEST_CASE("NxeLoader rejects a v3 image (clean cut, no back-compat)") {
	char buf[512];
	buildImage(buf);
	((NxHeader*) buf)->version = 3;
	nxaddr_t entry = 0;
	CHECK(NxeLoader::loadImage(buf, 512, 0, fakeResolve, &entry) == -1);
}
```

Uruchom — MUSI zfailować (loader wciąż 32-bitowy):
```bash
make test 2>&1 | tail -20
```
Expected: błędy kompilacji/asercji w `test_nxeloader` (sygnatura `entryOut` to `unsigned*`,
brak sprawdzenia wersji). To oczekiwane „czerwone".

- [ ] **Step 2: Zaimplementować loader 64-bit**

`kernel/NxeLoader.h` — `entryOut`/`loadDelta` na `nxaddr_t`, `addr` w `ExportFn` na
`nxaddr_t`:

```cpp
typedef void (*ExportFn)(void* ctx, const char* name, nxaddr_t addr);

class NxeLoader {
public:
	static int forEachNeeded(const void* image, unsigned len, NeededFn fn, void* ctx);
	static int loadImage(void* image, unsigned len, nxaddr_t loadDelta,
			ExportResolver resolve, nxaddr_t* entryOut,
			ExportFn onExport = 0, void* ctx = 0);
};
```

`kernel/NxeLoader.cpp` — `inImage` na `nxaddr_t`, sprawdzenie wersji, zapis relokacji jako
`*(nxaddr_t*)` (to R_X86_64_64 / R_386_32 — szerokość idzie za arch), slot IAT jako
`*(void**)` (8 bajtów na x86_64):

```cpp
#include "NxeLoader.h"
#include <string.h>

namespace kernel {

static bool inImage(nxaddr_t abs, nxaddr_t sz, nxaddr_t base, unsigned len) {
	if (abs < base) return false;
	nxaddr_t off = abs - base;
	return off + sz <= len && off + sz >= off;
}

int NxeLoader::forEachNeeded(const void* image, unsigned len, NeededFn fn, void* ctx) {
	if (len < sizeof(NxHeader)) return -1;
	const NxHeader* h = (const NxHeader*) image;
	if (h->magic != NX_MAGIC || h->version != NX_VERSION) return -1;
	const char* img = (const char*) image;
	nxaddr_t base = h->loadBase;
	if (!h->neededCount) return 0;
	if (!inImage(h->neededTable, (nxaddr_t) h->neededCount * sizeof(NxNeeded), base, len)) return -3;
	const NxNeeded* nd = (const NxNeeded*) (img + (h->neededTable - base));
	for (unsigned i = 0; i < h->neededCount; i++) {
		if (!inImage(nd[i].nameOff, 1, base, len)) return -3;
		if (fn) fn(ctx, img + (nd[i].nameOff - base));
	}
	return 0;
}

int NxeLoader::loadImage(void* image, unsigned len, nxaddr_t loadDelta,
		ExportResolver resolve, nxaddr_t* entryOut, ExportFn onExport, void* ctx) {
	if (len < sizeof(NxHeader)) return -1;
	NxHeader* h = (NxHeader*) image;
	if (h->magic != NX_MAGIC || h->version != NX_VERSION) return -1;   // reject v3 (clean cut)
	char* img = (char*) image;
	nxaddr_t base = h->loadBase;

	// 1) Base relocations: each listed absolute is an R_X86_64_64 (8-byte) word on x86_64,
	//    R_386_32 (4-byte) on i386 — nxaddr_t follows the arch, so one line serves both.
	if (h->relocCount) {
		if (!inImage(h->relocTable, (nxaddr_t) h->relocCount * sizeof(NxReloc), base, len)) return -3;
		NxReloc* rel = (NxReloc*) (img + (h->relocTable - base));
		for (unsigned i = 0; i < h->relocCount; i++) {
			if (!inImage(rel[i].off, sizeof(nxaddr_t), base, len)) return -3;
			*(nxaddr_t*) (img + (rel[i].off - base)) += loadDelta;
		}
	}

	// 2) Bind imports: patch each IAT slot (a function pointer — 8 bytes on x86_64).
	if (h->importCount) {
		if (!inImage(h->importTable, (nxaddr_t) h->importCount * sizeof(NxImport), base, len)) return -3;
		NxImport* imp = (NxImport*) (img + (h->importTable - base));
		for (unsigned i = 0; i < h->importCount; i++) {
			if (!inImage(imp[i].nameOff, 1, base, len)
					|| !inImage(imp[i].slotAddr, sizeof(void*), base, len)) return -3;
			if (imp[i].libOff && !inImage(imp[i].libOff, 1, base, len)) return -3;
			const char* name = (const char*) (img + (imp[i].nameOff - base));
			const char* lib = imp[i].libOff ? (const char*) (img + (imp[i].libOff - base)) : "";
			void* addr = resolve ? resolve(name, lib) : 0;
			if (addr == 0) return -2;
			*(void**) (img + (imp[i].slotAddr - base)) = addr;
		}
	}

	// 3) Exports: relocate each address, report it before bss-zero.
	if (h->exportCount) {
		if (!inImage(h->exportTable, (nxaddr_t) h->exportCount * sizeof(NxExport), base, len)) return -3;
		NxExport* ex = (NxExport*) (img + (h->exportTable - base));
		for (unsigned i = 0; i < h->exportCount; i++) {
			if (!inImage(ex[i].nameOff, 1, base, len)) return -3;
			ex[i].addr += loadDelta;
			if (onExport) onExport(ctx, img + (ex[i].nameOff - base), ex[i].addr);
		}
	}

	// 4) Zero bss last.
	if (h->bssEnd > h->bssStart) {
		if (!inImage(h->bssStart, h->bssEnd - h->bssStart, base, len)) return -3;
		memset(img + (h->bssStart - base), 0, (size_t) (h->bssEnd - h->bssStart));
	}

	if (entryOut) *entryOut = h->entry + loadDelta;
	return 0;
}

}
```

- [ ] **Step 3: Testy zielone**

```bash
make test 2>&1 | tail -20
```
Expected: wszystkie `NxeLoader` testy przechodzą (w tym nowy „rejects a v3 image"); bramka
pokrycia ≥90% utrzymana.

- [ ] **Step 4: i686 kernel buduje się (Exec.cpp używa loadera)**

`kernel/Exec.cpp` przekazuje `&entry` jako `entryOut`. Zmień `unsigned entry` →
`nxaddr_t entry` w `execProgram` i `execve` (dwa miejsca), bo `entryOut` to teraz
`nxaddr_t*`. Na i686 `nxaddr_t`=`uint32_t` (bez zmiany zachowania); `archEnterUser(entry,…)`
na i686 bierze `uint32_t` — zgodne. Wywołania `dynLoadProgram(...,&entry)` (DynLoader)
też dostają `nxaddr_t*` — jeśli DynLoader.h deklaruje `unsigned* entryOut`, zmień na
`nxaddr_t*` (jedna linia w `kernel/DynLoader.h` + definicja).

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build make build
```
Expected: i686 kernel się buduje.

- [ ] **Step 5: Commit**

```bash
git add tests/test_nxeloader.cpp kernel/NxeLoader.h kernel/NxeLoader.cpp kernel/Exec.cpp kernel/DynLoader.h kernel/DynLoader.cpp
git commit -m "nxe: 64-bit NxeLoader (R_X86_64_64 fixups, v4-only), entry as nxaddr_t"
```

---

## Task 5: mknx ELF64 + R_X86_64_* — host build mknx64

**Files:** Modify `tools/mknx.c`, Modify `Makefile`

- [ ] **Step 1: Zamienić ELF32 na ELF64 + makra/typy relokacji**

`mknx.c` jest host-narzędziem (native `cc`). Pod `-DNX_FORCE64` (build `mknx64`)
parsujemy ELF64 i zbieramy relokacje `R_X86_64_64`; bez flagi — stary ELF32/`R_386_32`
(i686). Zamień blok typów + makr (l. 28–55) na warunkowy:

```c
#include "NxFormat.h"   /* compiled with -Ikernel; -DNX_FORCE64 selects v4 layout */

#if defined(NX_FORCE64)
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize,
	e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf_Ehdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size;
	uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize; } Elf_Shdr;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx;
	uint64_t st_value, st_size; } Elf_Sym;
typedef struct { uint64_t r_offset, r_info; } Elf_Rel;   /* SHT_RELA also has r_addend after */
typedef struct { uint64_t r_offset, r_info, r_addend; } Elf_Rela;
#define SHT_RELA_T   4
#define R_ABS64      1     /* R_X86_64_64  — 8-byte absolute: a base-reloc site */
#define R_PC32       2     /* R_X86_64_PC32 — RIP-relative: NOT relocated (position-indep.) */
#define R_ABS32S     11    /* R_X86_64_32S — forces image into low 2 GiB; never a base-reloc */
#define EM_TARGET    62    /* EM_X86_64 */
#define ELF_R_TYPE(i) ((uint32_t)((i) & 0xffffffff))
#define ELF_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF_ST_BIND(i) ((i) >> 4)
#else
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
	e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
	e_shentsize, e_shnum, e_shstrndx; } Elf_Ehdr;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
	sh_link, sh_info, sh_addralign, sh_entsize; } Elf_Shdr;
typedef struct { uint32_t st_name, st_value, st_size; unsigned char st_info, st_other;
	uint16_t st_shndx; } Elf_Sym;
typedef struct { uint32_t r_offset, r_info; } Elf_Rel;
#define R_ABS64      1     /* R_386_32 */
#define R_PC32       2     /* R_386_PC32 */
#define EM_TARGET    3     /* EM_386 */
#define ELF_R_TYPE(i) ((i) & 0xff)
#define ELF_R_SYM(i)  ((i) >> 8)
#define ELF_ST_BIND(i) ((i) >> 4)
#endif
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_REL      9
#define SHT_NOBITS   8
#define SHF_ALLOC    2
#define SHF_EXECINSTR 4
#define STB_GLOBAL   1
#define STB_WEAK     2
#define SHN_UNDEF    0
#define SHN_ABS      0xfff1
```

Zamień każde `Elf32_Ehdr/Shdr/Sym/Rel` w ciele na `Elf_*`, `EM_386`→`EM_TARGET`,
`R_386_32`→`R_ABS64`, `R_386_PC32`→`R_PC32`, `ELF32_R_TYPE/SYM`→`ELF_R_TYPE/SYM`,
`ELF32_ST_BIND`→`ELF_ST_BIND`. Pola `loadBase/bssStart/bssEnd/imageSize/cur/…` zmień z
`uint32_t` na `nxaddr_t`. **Kluczowa zmiana semantyki relokacji** w pętli relokacyjnej:
x86_64 buduje sekcje `.rela.*` (`SHT_RELA`, z `r_addend`), nie `.rel.*`. Obsłuż OBA:

```c
/* 2) Base relocations. On x86_64 the linker emits SHT_RELA (with addend); on i386 SHT_REL.
 *    We record ONLY R_X86_64_64 sites against a DEFINED (section-relative) symbol — those
 *    hold an 8-byte module address that shifts with the load base. R_X86_64_PC32 is
 *    RIP-relative (position-independent) and never relocated. R_X86_64_32S appears in
 *    small-model non-PIC code and is NOT a base-reloc, but its presence is the reason the
 *    image MUST load in the low 2 GiB — which our fixed base 0x800000 guarantees. */
Buf relocs = {0};
unsigned relocCount = 0;
for (int i = 0; i < g_nsh; i++) {
	Elf_Shdr* s = sh(i);
	int isRela = (s->sh_type == SHT_RELA_T);   /* 0 on i386 build (macro guards below) */
#if !defined(NX_FORCE64)
	isRela = 0;
#endif
	if (s->sh_type != SHT_REL && !isRela) continue;
	Elf_Shdr* tgt = sh(s->sh_info);
	if (!(tgt->sh_flags & SHF_ALLOC)) continue;
	Elf_Sym* rsym = (Elf_Sym*) (g_elf + sh(s->sh_link)->sh_offset);
	unsigned char* base = g_elf + s->sh_offset;
	unsigned esz = isRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
	int n = (int) (s->sh_size / esz);
	for (int j = 0; j < n; j++) {
		uint64_t r_off, r_info;
		if (isRela) { Elf_Rela* r = (Elf_Rela*) (base + (size_t) j * esz); r_off = r->r_offset; r_info = r->r_info; }
		else        { Elf_Rel*  r = (Elf_Rel*)  (base + (size_t) j * esz); r_off = r->r_offset; r_info = r->r_info; }
		if (ELF_R_TYPE(r_info) != R_ABS64) continue;          /* only 8-byte absolute */
		if (r_off < loadBase || r_off >= bssStart) continue;
		uint16_t shndx = rsym[ELF_R_SYM(r_info)].st_shndx;
		if (shndx == SHN_UNDEF || shndx == SHN_ABS) continue; /* not a module address */
		bu64(&relocs, r_off);                                 /* nxaddr_t-wide reloc entry */
		relocCount++;
	}
}
```

Dodaj pomocnika `bu64` obok `bu32` (zapisuje `nxaddr_t`):
```c
static nxaddr_t bu64(Buf* b, nxaddr_t v) { unsigned o = b->len; bput(b, &v, sizeof v); return o; }
```
W blokach eksportów/importów/needed zamień `NxExport e = { nameOff, sym[i].st_value };`
itd. na pola `nxaddr_t` (kompilator dopasuje), a `bu32` używane do tablicy `relocs`
zastąp `bu64`. Auto-import (sekcja 4b) i sprawdzenie brakujących funkcji: ta sama logika,
`R_386_PC32`→`R_PC32`, parsując RELA gdy `NX_FORCE64`.

- [ ] **Step 2: Dodać host-build `mknx64` (Makefile)**

Obok reguły `$(MKNX)` (l. 819) dodaj wariant 64-bit (oba host-native `cc`):

```makefile
MKNX   = $(BINFOLDER)mknx
MKNX64 = $(BINFOLDER)mknx64
$(MKNX): tools/mknx.c kernel/NxFormat.h
	@mkdir -p $(BINFOLDER)
	cc -O2 -Wall -Ikernel -o $@ tools/mknx.c
$(MKNX64): tools/mknx.c kernel/NxFormat.h
	@mkdir -p $(BINFOLDER)
	cc -O2 -Wall -DNX_FORCE64 -Ikernel -o $@ tools/mknx.c
```

- [ ] **Step 3: Smoke-test mknx64 na realnym ELF64**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build sh -c '
  printf "void _start(){ for(;;); }\n" > /tmp/m.c &&
  x86_64-elf-gcc -ffreestanding -nostdlib -mcmodel=small -fno-pic -Wl,--emit-relocs \
    -Ttext=0x800000 -e _start -o /tmp/m.elf /tmp/m.c'
cc -O2 -Wall -DNX_FORCE64 -Ikernel -o bin/mknx64 tools/mknx.c
docker run --rm -v $(pwd):/src -w /src nanos-build x86_64-elf-readelf -h /tmp/m.elf | grep -E "Class|Machine"
./bin/mknx64 /tmp/m.elf /tmp/m.nxe && ls -l /tmp/m.nxe && echo OK
```
Expected: `Class: ELF64`, `Machine: …X86-64`; `mknx64` przechodzi (akceptuje `EM_X86_64`),
powstaje `/tmp/m.nxe`. (Sprawdź też: i686 `mknx` nadal buduje się i przetwarza ELF32 —
`make bin/mknx`.)

- [ ] **Step 4: Commit**

```bash
git add tools/mknx.c Makefile
git commit -m "mknx: ELF64 + R_X86_64_64/PC32/32S support (mknx64), keep ELF32 i386 path"
```

---

## Task 6: nxjmp64.S + nx.ld 64-bit + nxhdr (v4)

**Files:** Create `arch/x86_64/cpu/nxjmp64.S`, Create `arch/x86_64/user-nx.ld`,
Modify `arch/x86_64/arch.mk`

- [ ] **Step 1: `arch/x86_64/cpu/nxjmp64.S` — setjmp/longjmp dla exit()**

Zachowuje callee-saved System V (`rbx, rbp, r12–r15`) + `rsp` + powrotny `rip`; argument
w `rdi` (SysV), wartość zwrotna w `rax`.

```nasm
; arch/x86_64/cpu/nxjmp64.S — minimal setjmp/longjmp so a user program's exit() can
; return control to the kernel after it switched to its own stack (x86_64 SysV).
; struct NxJmp { u64 rsp, rbp, rbx, r12, r13, r14, r15, rip }  (offsets 0,8,16,...,56)
[BITS 64]
[GLOBAL nx_setjmp]
[GLOBAL nx_longjmp]

; long nx_setjmp(NxJmp* buf)  — rdi=buf; returns 0 here, returns `val` when longjmp'd to.
nx_setjmp:
    mov [rdi+0],  rsp        ; rsp points at the return address right now
    mov [rdi+8],  rbp
    mov [rdi+16], rbx
    mov [rdi+24], r12
    mov [rdi+32], r13
    mov [rdi+40], r14
    mov [rdi+48], r15
    mov rax, [rsp]           ; return address
    mov [rdi+56], rax
    xor eax, eax
    ret

; void nx_longjmp(NxJmp* buf, long val) — rdi=buf, rsi=val; resume at the setjmp site.
nx_longjmp:
    mov rax, rsi             ; return value
    mov rbp, [rdi+8]
    mov rbx, [rdi+16]
    mov r12, [rdi+24]
    mov r13, [rdi+32]
    mov r14, [rdi+40]
    mov r15, [rdi+48]
    mov rsp, [rdi+0]         ; restore stack ([rsp] = saved return address)
    mov rdx, [rdi+56]
    mov [rsp], rdx           ; ensure we ret to the setjmp site
    ret
```

- [ ] **Step 2: `arch/x86_64/user-nx.ld` — skrypt linkera user (baza niska, elf64)**

Baza `0x800000` (niska, kanoniczna, w dolnych 2 GiB — decyzja #1). `OUTPUT_FORMAT` elf64.

```ld
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS
{
  . = 0x800000;                  /* fixed low base (< 2 GiB) — satisfies R_X86_64_32S */
  .nxheader  : { KEEP(*(.nxheader)) }
  .text      : { *(.text*) }
  .nximports : { *(.nximports) }
  .rodata    : { *(.rodata*) }
  .data      : { *(.data*) }
  .nxlib.libc.ndl  : { *(.nxlib.libc.ndl) }
  __bss_start = .;
  .bss       : { *(.bss*) *(COMMON) }
  __bss_end = .;
  __nx_image_size = __bss_start - 0x800000;
}
```

- [ ] **Step 3: `arch/x86_64/arch.mk` — dołożyć MD źródła Planu 6**

Zamień linię `ARCH_SOURCES=...` na pełen zestaw (dodaje syscall/usermode/nxjmp; zachowuje
to, co dołożyły Plany 2–5 — wstaw spójnie z aktualną zawartością po Planie 5):

```makefile
# Plan 6 adds the ring-3 path: syscall trap (MSR init + entry stub), usermode enter,
# the exit() longjmp. Append to whatever Plans 2-5 already listed.
ARCH_SOURCES += syscall_x86_64.o syscall_entry64.o usermode_x86_64.o nxjmp64.o
```

- [ ] **Step 4: Sprawdzić asemblację nxjmp64 + parsowanie linkera**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build sh -c '
  nasm -f elf64 arch/x86_64/cpu/nxjmp64.S -o /tmp/nxjmp64.o &&
  x86_64-elf-ld -T arch/x86_64/user-nx.ld --print-output-format && echo OK'
```
Expected: brak błędów; `elf64-x86-64`; `OK`.

- [ ] **Step 5: Commit**

```bash
git add arch/x86_64/cpu/nxjmp64.S arch/x86_64/user-nx.ld arch/x86_64/arch.mk
git commit -m "x86_64: add nxjmp64 (setjmp/longjmp) + 64-bit user linker script"
```

---

## Task 7: syscall_entry64.S — stub wejścia syscall (swapgs + ramka)

**Files:** Create `arch/x86_64/cpu/syscall_entry64.S`

- [ ] **Step 1: Napisać stub wejścia**

`syscall` NIE przełącza stosu i wchodzi z IF=0 (FMASK kasuje IF). Stub: `swapgs`
(GS→kernel per-CPU), zapis user `rsp` do per-CPU scratch, załadowanie kernelowego `rsp`,
zbudowanie ramki zgodnej z `kernel::Registers` (Plan 4), wołanie C `syscall_dispatch64`,
odtworzenie i `sysretq`. `rcx`=zapisany RIP, `r11`=RFLAGS (wymóg `sysret`).

Per-CPU (GS po swapgs): offset 0 = kernelowy top stosu, offset 8 = scratch na user rsp.

```nasm
; arch/x86_64/cpu/syscall_entry64.S — SYSCALL fast-path entry (NASM elf64).
; Entry state (set by the CPU): rcx = user RIP, r11 = user RFLAGS, IF cleared by FMASK,
; CS/SS from STAR, STILL ON THE USER STACK. We swapgs to the kernel per-CPU block, switch
; to the task's kernel stack, build a kernel::Registers frame, and call into C.
;
; Per-CPU layout reached via %gs after swapgs (set in syscall_x86_64.cpp):
;   [gs:0] = kernel stack top for the current task    [gs:8] = scratch for the user rsp
[BITS 64]
[GLOBAL syscall_entry]
[EXTERN syscall_dispatch64]      ; void syscall_dispatch64(kernel::Registers*)

syscall_entry:
    swapgs                       ; GS.base -> kernel per-CPU (IA32_KERNEL_GS_BASE)
    mov [gs:8], rsp              ; stash the user rsp
    mov rsp, [gs:0]              ; switch to the current task's kernel stack

    ; Build a trap frame matching kernel::Registers (Plan 4). Push the SS/RSP/RFLAGS/CS/RIP
    ; tail an iretq-style frame would have, synthesized from the syscall-saved regs, so the
    ; SAME signal/exec code (archFrameToUser/archSigreturn) can manipulate it.
    push qword 0x23              ; ss  (user data, DPL3)
    push qword [gs:8]            ; rsp (the saved user rsp)
    push r11                     ; rflags (saved by syscall)
    push qword 0x2B              ; cs  (user code64, DPL3)
    push rcx                     ; rip (return address saved by syscall)
    push qword 0                 ; err_code (none for a syscall)
    push qword 0x100             ; int_no = 0x100 marks "via syscall" (distinct from int 0x80)

    push rax                     ; --- GPRs, order MUST match kernel::Registers ---
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                 ; arg0 = &Registers
    cld
    call syscall_dispatch64

    pop r15                      ; restore GPRs (rax now holds the syscall result)
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16                  ; drop int_no + err_code
    pop rcx                      ; rip  -> rcx for sysret
    add rsp, 8                   ; skip cs
    pop r11                      ; rflags -> r11 for sysret
    pop rsp                      ; restore user rsp (skip nothing else: ss was above it)
    ; NB: after `pop rcx`(rip),`+8`(cs),`pop r11`(rflags), top is rsp then ss; `pop rsp`
    ; loads the user rsp and discards the ss slot implicitly (it was pushed below rsp).
    swapgs                       ; GS.base -> user
    sysretq                      ; CS=STAR[63:48]+16|3, SS=+8|3, RIP=rcx, RFLAGS=r11
```

> Kolejność `push`/`pop` GPR i ogona (`int_no/err/rip/cs/rflags/rsp/ss`) MUSI być
> identyczna jak w `kernel::Registers` z Planu 4 — jeśli Plan 4 ułożył pola inaczej,
> dopasuj tu (to jedyna twarda zależność layoutu). `int_no=0x100` pozwala wspólnemu kodowi
> sygnałów odróżnić wejście przez `syscall` (restart cofa o 2 bajty `syscall`) od `int`.

- [ ] **Step 2: Asemblacja**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build nasm -f elf64 arch/x86_64/cpu/syscall_entry64.S -o /tmp/se.o && echo OK
```
Expected: `OK`.

- [ ] **Step 3: Commit**

```bash
git add arch/x86_64/cpu/syscall_entry64.S
git commit -m "x86_64: SYSCALL entry stub (swapgs, kernel stack, Registers frame, sysretq)"
```

---

## Task 8: syscall_x86_64.cpp — init MSR + dekodowanie + self-test

**Files:** Create `arch/x86_64/cpu/syscall_x86_64.cpp`

- [ ] **Step 1: Napisać implementację `<arch/syscall.h>`**

```cpp
/*
 * syscall_x86_64.cpp — x86_64 syscall trap: implements <arch/syscall.h>.
 *
 * SYSCALL/SYSRET (not int 0x80). syscallInit programs the MSRs (EFER.SCE, STAR, LSTAR,
 * FMASK) and the per-CPU block GS points at after swapgs. The asm stub (syscall_entry64.S)
 * builds a kernel::Registers frame and calls syscall_dispatch64, which decodes the AMD64
 * SysV registers (nr in rax; args rdi/rsi/rdx/r10/r8/r9) and forwards to the MI
 * kernel::kernelSyscall. Mirrors arch/x86/cpu/syscall_x86.cpp's syscallTrap.
 */
#include <arch/syscall.h>
#include <arch/usermode.h>
#include <arch/cpu.h>
#include "Interrupt.h"          // kernel::Registers (x86_64 TrapFrame, from Plan 4)
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "SignalDispatch.h"
#include "Exec.h"
#include <stdint.h>

namespace arch {
// Defined here, updated by the scheduler / archEnterUser: the kernel stack top the syscall
// stub loads (per-CPU). For a single CPU one block suffices.
struct PerCpu { uint64_t kernelStackTop; uint64_t userRspScratch; };
PerCpu g_percpu;
void syscallSetKernelStack(uint64_t top) { g_percpu.kernelStackTop = top; }
}

namespace {

// MSR numbers.
const uint32_t IA32_EFER          = 0xC0000080;
const uint32_t IA32_STAR          = 0xC0000081;
const uint32_t IA32_LSTAR         = 0xC0000082;
const uint32_t IA32_FMASK         = 0xC0000084;
const uint32_t IA32_KERNEL_GS_BASE= 0xC0000102;

static inline void wrmsr(uint32_t msr, uint64_t v) {
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t) hi << 32) | lo;
}

}  // namespace

extern "C" void syscall_entry();   // syscall_entry64.S

extern "C" void syscall_dispatch64(kernel::Registers* r) {
	// SYSCALL enters with IF=0 (FMASK clears it). Re-enable interrupts for the body, exactly
	// like the int 0x80 path: deferred preemption keeps the kernel non-preemptible, and the
	// sysret restores the caller's RFLAGS (saved in r->rflags / r11).
	arch::cpuEnableInterrupts();
	uint64_t origRax = r->rax;     // syscall number, saved before dispatch (for restart)
	// AMD64 SysV syscall ABI: nr=rax, args=rdi,rsi,rdx,r10,r8,r9.
	long ret = kernel::kernelSyscall((int) r->rax, r->rdi, r->rsi, r->rdx, r->r10, r->r8,
			r->r9, (arch::TrapFrame*) r);
	r->rax = (uint64_t) ret;
	if (kernel::kernelSyscalls()->hasExited())
		kernel::procExit();                          // does not return
	if ((r->cs & 3) == 3)
		kernel::signalDeliver((arch::TrapFrame*) r, (unsigned) origRax, true);
}

namespace arch {

void syscallInit() {
	// 1) Enable SYSCALL/SYSRET (EFER.SCE = bit 0).
	wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1);
	// 2) STAR: SYSCALL loads CS=STAR[47:32], SS=+8 (kernel 0x08/0x10); SYSRET computes user
	//    selectors from STAR[63:48] (=0x18 -> CS 0x2B, SS 0x23). See Task 0 GDT layout.
	wrmsr(IA32_STAR, ((uint64_t) 0x08 << 32) | ((uint64_t) 0x18 << 48));
	// 3) LSTAR: the entry RIP.
	wrmsr(IA32_LSTAR, (uint64_t) &syscall_entry);
	// 4) FMASK: bits cleared in RFLAGS on entry. Clear IF (no nested IRQ until we re-enable)
	//    and DF (SysV requires DF=0 in the kernel).
	wrmsr(IA32_FMASK, (1 << 9) | (1 << 10));   // IF | DF
	// 5) KERNEL_GS_BASE -> the per-CPU block the stub reads after swapgs.
	wrmsr(IA32_KERNEL_GS_BASE, (uint64_t) &g_percpu);
}

void syscallSelfTest() {
	// A zero-length write(1, "", 0) round-trips through the real syscall path without
	// printing — catches a broken MSR/entry/stub before userland exists.
	long ret;
	const char* msg = "";
	__asm__ __volatile__("syscall"
		: "=a"(ret)
		: "a"((long) SYS_write), "D"(1L), "S"((long) msg), "d"(0L)
		: "rcx", "r11", "memory");
	(void) ret;
}

}  // namespace arch
```

> `syscallSelfTest` wykonuje `syscall` z ringu 0 — by zadziałało, KERNEL_GS_BASE i
> per-CPU `kernelStackTop` muszą być ustawione PRZED nim. `syscallInit` ustawia GS base;
> `kernelStackTop` ustaw na dedykowany bufor LUB pomiń self-test do pierwszego
> `archEnterUser` (gdzie `syscallSetKernelStack` jest wołane). Najprościej: w `syscallInit`
> ustaw `g_percpu.kernelStackTop` na bieżący `rsp` (kernel boot stack) — wystarcza do
> zerowego self-testu z ringu 0.

- [ ] **Step 2: Zbudować obiekt (flagi kernela x86_64)**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Iinclude -Iarch/x86_64/cpu \
  arch/x86_64/cpu/syscall_x86_64.cpp -o /tmp/sc.o && echo OK
```
Expected: `OK`. (Jeśli brakuje `kernel::Registers` pól `rax/rdi/...` — to znak, że layout
z Planu 4 trzeba zsynchronizować; patrz Task 0.)

- [ ] **Step 3: Commit**

```bash
git add arch/x86_64/cpu/syscall_x86_64.cpp
git commit -m "x86_64: syscall MSR init (EFER/STAR/LSTAR/FMASK) + AMD64 SysV arg decode"
```

---

## Task 9: usermode_x86_64.cpp — wejście w ring 3 + %fs.base + ramka sygnału

**Files:** Create `arch/x86_64/cpu/usermode_x86_64.cpp`, Modify `arch/include/arch/usermode.h`

- [ ] **Step 1: Poszerzyć `<arch/usermode.h>` na adresy 64-bit**

Adresowe parametry `uint32_t`→`uint64_t` (per-arch identyczne, bo i686 też dostanie
64-bit sygnaturę — jego implementacja zwęża wewnętrznie; ale by NIE ruszać i686, użyj
`uintptr_t`, które jest 32-bit na i686 / 64-bit na x86_64). Zamień sygnatury:

```cpp
uintptr_t archLoadUser(AddressSpace* space, uintptr_t loadBase, uintptr_t bssEnd,
                       const char* const* argv, int argc,
                       const char* const* envp, int envc);
void archEnterUser(uintptr_t entry, uintptr_t userEsp, AddressSpace* space);
void archLoadModule(AddressSpace* space, uintptr_t base, const void* img, uintptr_t bytes);
void archFrameToUser(TrapFrame* tf, uintptr_t entry, uintptr_t userEsp);
void archPushSignalFrame(TrapFrame* tf, uintptr_t handler, uintptr_t restorer,
                         int sig, uint64_t oldMask, uintptr_t origEax, int restartAction);
int  archSigreturn(TrapFrame* tf, uint64_t* oldMaskOut);
int  archSyscallResult(TrapFrame* tf);
void archRestartSyscall(TrapFrame* tf, uintptr_t origEax);
// NEW on x86_64: set the user thread pointer (%fs.base) for the current/next ring-3 run.
void archSetUserFsBase(uint64_t base);
```

> `uintptr_t` jest 32-bit na i686-elf, więc `arch/x86/cpu/usermode_x86.cpp` (które bierze
> `uint32_t`) dalej się kompiluje BEZ zmian (uint32_t == uintptr_t na i686). `archSetUserFsBase`
> dostaje pustą implementację na i686 (TLS przez GDT %gs) — dodaj 1-linijkowy stub w
> `usermode_x86.cpp`: `void archSetUserFsBase(uint64_t) {}`.

- [ ] **Step 2: Napisać `usermode_x86_64.cpp`**

```cpp
/*
 * usermode_x86_64.cpp — x86_64 implementation of <arch/usermode.h>.
 *
 * archLoadUser maps a staged image + a SysV-AMD64 argv/envp stack into a per-process
 * space. archEnterUser sets %fs.base (decision #2 — picolibc TLS errno), switches CR3 and
 * iretq's to ring 3. The signal frame carries all 64-bit GPRs (~2x the i386 one).
 */
#include <arch/usermode.h>
#include <arch/mmu.h>
#include "PagingControl.h"
#include "FrameAllocator.h"
#include "Interrupt.h"          // kernel::Registers (x86_64 TrapFrame)
#include <string.h>
#include <stdint.h>

namespace arch { void syscallSetKernelStack(uint64_t top); }   // syscall_x86_64.cpp

namespace {
// User window mirrors i686 but in 64-bit low-canonical VA: image at loadBase=0x800000,
// stack at the top of the 8 MiB window. (Plan 3's mmu_x86_64 reserves this band.)
const uint64_t USER_STACK_TOP = 0x1000000;
const uint64_t USER_STACK_BOT = 0xF80000;   // 512 KiB stack

const uint32_t IA32_FS_BASE = 0xC0000100;
inline void wrmsr(uint32_t msr, uint64_t v) {
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}

// Build a SysV-AMD64 argv/envp image at the top of the user stack. Layout (high->low):
// the strings; then a 16-byte-aligned block: argc(8), argv ptrs(8 each)+NULL, envp+NULL.
// rsp ends pointing at argc and MUST be 16-aligned at _start (ABI). Writing is delegated so
// we can poke the child's frames by physical address.
template <class WriteFn>
uint64_t buildUserStack64(uint64_t top, const char* const* argv, int argc,
		const char* const* envp, int envc, WriteFn write) {
	uint64_t sp = top;
	uint64_t aptr[128], eptr[128];
	for (int i = envc - 1; i >= 0; i--) { unsigned l = 0; while (envp[i][l]) l++; l++; sp -= l; write(sp, envp[i], l); eptr[i] = sp; }
	for (int i = argc - 1; i >= 0; i--) { unsigned l = 0; while (argv[i][l]) l++; l++; sp -= l; write(sp, argv[i], l); aptr[i] = sp; }
	uint64_t slots = (uint64_t) (1 + (argc + 1) + (envc + 1));   // argc + argv[]+NULL + envp[]+NULL
	sp -= slots * 8;
	sp &= ~0xFull;                       // 16-align the block base (= rsp at _start)
	uint64_t off = sp;
	uint64_t v = (uint64_t) argc; write(off, &v, 8); off += 8;
	for (int i = 0; i < argc; i++) { write(off, &aptr[i], 8); off += 8; }
	uint64_t nul = 0; write(off, &nul, 8); off += 8;
	for (int i = 0; i < envc; i++) { write(off, &eptr[i], 8); off += 8; }
	write(off, &nul, 8);
	return sp;
}
}  // namespace

namespace arch {

uintptr_t archLoadUser(AddressSpace* space, uintptr_t loadBase, uintptr_t bssEnd,
		const char* const* argv, int argc, const char* const* envp, int envc) {
	uint64_t imgEnd = (bssEnd + 0xFFF) & ~0xFFFull;
	for (uint64_t va = loadBase; va < imgEnd; va += 0x1000) {
		uint64_t f = kernel::g_frames.alloc();
		memcpy((void*) f, (void*) va, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}
	uint64_t stackFrames[128];
	int sfi = 0;
	for (uint64_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += 0x1000) {
		uint64_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
		stackFrames[sfi++] = f;
	}
	auto stackPhys = [&](uint64_t va) -> uint64_t {
		return stackFrames[(va - USER_STACK_BOT) >> 12] + (va & 0xFFFull);
	};
	return (uintptr_t) buildUserStack64(USER_STACK_TOP, argv, argc, envp, envc,
		[&](uint64_t va, const void* src, unsigned len) {
			const unsigned char* s = (const unsigned char*) src;
			for (unsigned i = 0; i < len; i++) *(unsigned char*) stackPhys(va + i) = s[i];
		});
}

void archLoadModule(AddressSpace* space, uintptr_t base, const void* img, uintptr_t bytes) {
	const unsigned char* src = (const unsigned char*) img;
	uint64_t end = (bytes + 0xFFF) & ~0xFFFull;
	for (uint64_t off = 0; off < end; off += 0x1000) {
		uint64_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		uint64_t n = bytes - off < 0x1000 ? bytes - off : 0x1000;
		memcpy((void*) f, src + off, (size_t) n);
		mmuMap(space, base + off, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}
}

void archSetUserFsBase(uint64_t base) { wrmsr(IA32_FS_BASE, base); }

void archEnterUser(uintptr_t entry, uintptr_t userRsp, AddressSpace* space) {
	__asm__ __volatile__("cli");
	mmuSwitch(space);
	// The syscall stub will run on THIS task's kernel stack — point the per-CPU block at it.
	uint64_t krsp; __asm__ __volatile__("mov %%rsp, %0" : "=r"(krsp));
	syscallSetKernelStack(krsp & ~0xFull);
	// %fs.base for picolibc TLS (decision #2). loadBase is fine as a placeholder thread
	// pointer for the minimal init (no real TLS); a picolibc program sets it via arch_prctl.
	archSetUserFsBase((uint64_t) entry & 0);   // 0 until a TLS block exists; see note below
	// iretq down to ring 3: push SS, RSP, RFLAGS(IF=1), CS, RIP.
	__asm__ __volatile__(
		"mov $0x23, %%ax\n\t"     // user data selector (DPL3)
		"mov %%ax, %%ds\n\t"
		"mov %%ax, %%es\n\t"
		"pushq $0x23\n\t"         // ss
		"pushq %0\n\t"            // rsp
		"pushq $0x202\n\t"        // rflags (IF=1)
		"pushq $0x2B\n\t"         // cs (user code64, DPL3)
		"pushq %1\n\t"            // rip
		"iretq\n\t"
		:: "r"((uint64_t) userRsp), "r"((uint64_t) entry) : "rax", "memory");
	// not reached
}

namespace {
// Saved on the user stack to resume the interrupted context after a handler (x86_64).
struct SigContext {
	uint64_t rip, rflags, rsp, rbp;
	uint64_t rax, rbx, rcx, rdx, rsi, rdi;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t oldmask;
};
}

void archPushSignalFrame(TrapFrame* tf, uintptr_t handler, uintptr_t restorer,
		int sig, uint64_t oldMask, uintptr_t origRax, int restartAction) {
	kernel::Registers* r = (kernel::Registers*) tf;
	uint64_t usp = r->rsp;
	uint64_t resumeRip = r->rip, resumeRax = r->rax;
	if (restartAction == SIG_FRAME_RESTART) { resumeRip = r->rip - 2; resumeRax = origRax; }  // back over `syscall` (2 bytes)
	else if (restartAction == SIG_FRAME_EINTR) { resumeRax = (uint64_t) (-4L); }              // -EINTR

	usp &= ~0xFull;                          // keep the user stack 16-aligned
	usp -= sizeof(SigContext);
	SigContext* ctx = (SigContext*) usp;
	ctx->rip = resumeRip; ctx->rflags = r->rflags; ctx->rsp = r->rsp; ctx->rbp = r->rbp;
	ctx->rax = resumeRax; ctx->rbx = r->rbx; ctx->rcx = r->rcx; ctx->rdx = r->rdx;
	ctx->rsi = r->rsi;    ctx->rdi = r->rdi; ctx->r8 = r->r8;   ctx->r9 = r->r9;
	ctx->r10 = r->r10;    ctx->r11 = r->r11; ctx->r12 = r->r12; ctx->r13 = r->r13;
	ctx->r14 = r->r14;    ctx->r15 = r->r15; ctx->oldmask = oldMask;

	usp -= 8; *(uint64_t*) usp = restorer;   // handler's return address -> sigtramp
	r->rdi = (uint64_t) sig;                 // SysV arg1 = signum
	r->rip = handler;
	r->rsp = usp;
	r->rflags &= ~0x400ull;                  // clear DF for the handler
}

int archSigreturn(TrapFrame* tf, uint64_t* oldMaskOut) {
	kernel::Registers* r = (kernel::Registers*) tf;
	const SigContext* ctx = (const SigContext*) r->rsp;   // sigtramp left rsp at the context
	uint64_t savedRax = ctx->rax;
	r->rip = ctx->rip;
	r->rflags = (ctx->rflags & 0xCD5ull) | 0x202ull;
	r->rbx = ctx->rbx; r->rcx = ctx->rcx; r->rdx = ctx->rdx; r->rsi = ctx->rsi; r->rdi = ctx->rdi;
	r->rbp = ctx->rbp; r->r8 = ctx->r8; r->r9 = ctx->r9; r->r10 = ctx->r10; r->r11 = ctx->r11;
	r->r12 = ctx->r12; r->r13 = ctx->r13; r->r14 = ctx->r14; r->r15 = ctx->r15;
	r->rsp = ctx->rsp; r->rax = savedRax;
	if (oldMaskOut) *oldMaskOut = ctx->oldmask;
	return (int) savedRax;
}

int archSyscallResult(TrapFrame* tf) { return (int) ((kernel::Registers*) tf)->rax; }

void archRestartSyscall(TrapFrame* tf, uintptr_t origRax) {
	kernel::Registers* r = (kernel::Registers*) tf;
	r->rip -= 2;            // back over the 2-byte `syscall`
	r->rax = origRax;
}

void archFrameToUser(TrapFrame* tf, uintptr_t entry, uintptr_t userRsp) {
	kernel::Registers* r = (kernel::Registers*) tf;
	r->rip = entry; r->rsp = userRsp;
	r->cs = 0x2B; r->ss = 0x23;
	r->rflags = 0x202;     // IF=1
	r->rax = 0;
}

}  // namespace arch
```

> Uwaga o `archSetUserFsBase` w `archEnterUser`: minimalny `init.nxe` (Task 13) NIE używa
> picolibc/TLS, więc `%fs.base` może zostać 0. Realny program picolibc ustawia thread
> pointer przez `arch_prctl(ARCH_SET_FS)` (numer w SyscallNr) — dodaj `case SYS_arch_prctl`
> w dispatchu (1 linia: `archSetUserFsBase(a1)` dla `ARCH_SET_FS=0x1002`) oraz wywołanie
> `archSetUserFsBase(thread->tlsBase)` przy przełączeniu kontekstu na zadanie ringu-3 w
> schedulerze Planu 4. To pełna ścieżka #2; tu zostawiamy haczyk + komentarz, bo realny TLS
> przychodzi z picolibc w Planie 8.

- [ ] **Step 3: Zbudować obiekt**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Iinclude -Iarch/x86_64/cpu -Iarch/x86_64/mm \
  arch/x86_64/cpu/usermode_x86_64.cpp -o /tmp/um.o && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add arch/include/arch/usermode.h arch/x86/cpu/usermode_x86.cpp arch/x86_64/cpu/usermode_x86_64.cpp
git commit -m "x86_64: usermode — iretq to ring3, %fs.base, 64-bit SysV stack + signal frame"
```

---

## Task 10: Userland build x86_64 — flagi SSE + crt0 + libnanos + sigtramp

**Files:** Create `user/crt064.S`, Create `user/sigtramp64.S`, Modify `user/libnanos.c`,
Modify `Makefile`

- [ ] **Step 1: `user/crt064.S` — _start SysV AMD64**

```nasm
; user/crt064.S — NanOS user startup, System V AMD64. The kernel enters at _start with rsp
; pointing at argc; argv[] follows, then NULL, then envp[]. Pass main(argc, argv, envp) in
; rdi/rsi/rdx, keep rsp 16-aligned at the call, then exit(main(...)).
[BITS 64]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
_start:
    xor rbp, rbp
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    lea rdx, [rsp + 8 + rdi*8 + 8]  ; envp = argv + (argc+1)*8
    and rsp, -16                ; 16-align before the call (ABI)
    call main                   ; main(argc, argv, envp) -> rax
    mov rdi, rax                ; exit code
    call exit                   ; does not return (kernel longjmps back)
.hang:
    jmp .hang
```

- [ ] **Step 2: `user/sigtramp64.S` — sigreturn trampoline x86_64**

```nasm
; user/sigtramp64.S — the kernel enters the handler with [rsp] = this trampoline (its return
; address) and the SigContext just below. handler(signum) returns here; rsp then points at
; the SigContext, so invoke rt_sigreturn, which restores the interrupted context.
[BITS 64]
[GLOBAL __nx_sigtramp]
%include "user/sysnr.inc"          ; SYS_rt_sigreturn (generated, see Step 4)
__nx_sigtramp:
    mov rax, SYS_RT_SIGRETURN
    syscall
.hang:
    jmp .hang
```

> NASM nie czyta `SyscallNr.h` (C). Najprościej: `%define SYS_RT_SIGRETURN 15` wprost w
> pliku (x86_64 `rt_sigreturn`=15) z komentarzem „musi == SyscallNr.h"; pomiń `%include`.
> Zaktualizuj plik tak, by miał `%define SYS_RT_SIGRETURN 15` i NIE `%include`.

- [ ] **Step 3: `user/libnanos.c` — wrappery przez `syscall` na x86_64**

Owiń `syscall3` w `#ifdef __x86_64__`; argumenty/typy na `long`; klobry `rcx,r11`:

```c
#include "libnanos.h"
#include "SyscallNr.h"

#if defined(__x86_64__)
static inline long syscall3(long nr, long a0, long a1, long a2) {
	long ret;
	__asm__ __volatile__("syscall"
		: "=a"(ret)
		: "a"(nr), "D"(a0), "S"(a1), "d"(a2)
		: "rcx", "r11", "memory");
	return ret;
}
#else
static inline int syscall3(int nr, int a0, int a1, int a2) {
	int ret;
	__asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(nr), "b"(a0), "c"(a1), "d"(a2) : "memory");
	return ret;
}
#endif

int write(int fd, const void* b, unsigned n) { return (int) syscall3(SYS_write, fd, (long) b, n); }
int read(int fd, void* b, unsigned n)         { return (int) syscall3(SYS_read, fd, (long) b, n); }
int open(const char* p, int f)                { return (int) syscall3(SYS_open, (long) p, f, 0); }
int close(int fd)                             { return (int) syscall3(SYS_close, fd, 0, 0); }
void exit(int c)                              { syscall3(SYS_exit, c, 0, 0); for (;;) {} }
```

> `(long) b` na x86_64 jest pełnym 64-bitowym wskaźnikiem (kontra `(int)` na i686, gdzie
> wskaźnik jest 32-bit). To naprawia obcięcie wskaźników, którego prompt #1/§4 dotyczy.

- [ ] **Step 4: Makefile — x86_64 userland (flagi SSE) + mknx64 + cel `init64`**

Dodaj w sekcji userlandu (po `USER_CFLAGS`, l. 690) zestaw flag x86_64 dla MINIMALNEGO
init (freestanding, BEZ picolibc — picolibc/libc.ndl to Plan 8) i regułę linkowania:

```makefile
# x86_64 minimal in-tree userland (Plan 6): freestanding, SSE ON (decision #3 — SysV AMD64
# requires SSE for varargs/float; the kernel keeps -mno-sse, see arch/x86_64/arch.mk). Small
# code model, non-PIC, fixed low base (decision #1). Built by the KERNEL toolchain.
UARCHFLAGS64 = -mcmodel=small -fno-pic -mno-red-zone   # SSE intentionally NOT disabled
USER64_CFLAGS = -ffreestanding -nostdlib -nostdinc -Ikernel -Iuser $(UARCHFLAGS64) -Wall

# Build the 64-bit init.nxe: crt0 + nxhdr + libnanos + init, linked at 0x800000, then mknx64.
bin/init64.elf: user/crt064.o user/nxhdr64.o user/libnanos64.o user/init64.o arch/x86_64/user-nx.ld
	$(CROSS)gcc -nostdlib -Wl,--emit-relocs -T arch/x86_64/user-nx.ld \
	  -o $@ user/crt064.o user/nxhdr64.o user/libnanos64.o user/init64.o
bin/init.nxe: bin/init64.elf $(MKNX64)
	$(MKNX64) $< $@

# Object rules for the 64-bit userland (distinct *64.o names avoid clashing with the i686
# bin/*.o during transition; the 64-suffix convention, like loader64.o in Plan 1).
user/crt064.o: user/crt064.S
	nasm -f elf64 $< -o $@
user/sigtramp64.o: user/sigtramp64.S
	nasm -f elf64 $< -o $@
user/nxhdr64.o: user/nxhdr.c
	$(CROSS)gcc $(USER64_CFLAGS) -c $< -o $@
user/libnanos64.o: user/libnanos.c
	$(CROSS)gcc $(USER64_CFLAGS) -c $< -o $@
user/init64.o: user/init64.c
	$(CROSS)gcc $(USER64_CFLAGS) -c $< -o $@

# Host-side helper: build the 64-bit init bundle (container builds objects + .nxe).
.PHONY: init64
init64:
	$(DOCKER_RUN) make ARCH=x86_64 bin/init.nxe
```

> `bin/*.o` dla obiektów 64-bit kolidowałyby z i686 podczas przejścia, więc kładziemy je
> obok źródeł jako `user/*64.o` (poza `bin/`). To różni się od głównej reguły userlandu;
> docelowo (Plan 8) i686 znika i userland wraca do `bin/`. `init.nxe` zapisywany do obrazu
> przez istniejący `_image` (`/nanos/core/init.nxe`).

- [ ] **Step 5: `user/init64.c` — minimalny program (cat grub.cfg + exit)**

```c
/* user/init64.c — minimal 64-bit PID 1 proof: open + read + write grub.cfg, then exit.
 * Mirrors the historical i686 init that cats grub.cfg through the imported API. No libc —
 * just the libnanos syscall wrappers (the picolibc/nsh path is Plan 8). */
#include "libnanos.h"
int main(int argc, char** argv, char** envp) {
	(void) argc; (void) argv; (void) envp;
	const char* hello = "NanOS x86_64 userland: ring 3 + syscall OK\n";
	unsigned n = 0; while (hello[n]) n++;
	write(1, hello, n);
	int fd = open("/disks/main/boot/grub/grub.cfg", 0);   // O_RDONLY
	if (fd >= 0) {
		char buf[512];
		int r;
		while ((r = read(fd, buf, sizeof buf)) > 0)
			write(1, buf, (unsigned) r);
		close(fd);
	}
	exit(0);
	return 0;
}
```

- [ ] **Step 6: Zbudować init.nxe**

```bash
make init64
docker run --rm -v $(pwd):/src -w /src nanos-build sh -c '
  x86_64-elf-readelf -h bin/init64.elf | grep -E "Class|Machine|Entry" && ls -l bin/init.nxe'
```
Expected: `ELF64`, `…X86-64`, entry `0x8000xx`; powstaje `bin/init.nxe` (v4 — `mknx64`).

- [ ] **Step 7: Commit**

```bash
git add user/crt064.S user/sigtramp64.S user/libnanos.c user/init64.c Makefile
git commit -m "x86_64 userland: SysV crt0, syscall wrappers, minimal init.nxe (cat grub.cfg)"
```

---

## Task 11: Wpięcie syscallInit w boot + zbudowanie pełnego kernela x86_64

**Files:** (zależne od Planu 2/4 wiring) Modify `kernel/Kernel.cpp` lub arch boot

- [ ] **Step 1: Wywołać `arch::syscallInit()` w starcie kernela**

W `Kernel::start` (po `cpuInit`/IDT z Planu 4, przed `execProgram`) upewnij się, że jest
`arch::syscallInit();` oraz `arch::installSyscalls(vfs)` (jak na i686). Na x86_64
`syscallInit` programuje MSR-y; `syscallSelfTest` (opcjonalnie) sprawdza round-trip. Jeśli
`Kernel.cpp` woła `syscallInit` już od i686 — nic nie dodajemy (MI), tylko potwierdzamy.

- [ ] **Step 2: Zbudować PEŁNY kernel x86_64 (MI + MD razem)**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build make ARCH=x86_64 build
```
Expected: `bin/kernel.bin` (elf64) linkuje się z nowymi `syscall_x86_64.o`,
`syscall_entry64.o`, `usermode_x86_64.o`, `nxjmp64.o` + całym MI. (Błędy braku symboli/pól
`Registers` → synchronizacja z Planem 4, Task 0.)

- [ ] **Step 3: Commit**

```bash
git add kernel/Kernel.cpp
git commit -m "x86_64: wire arch::syscallInit() into boot; full kernel links with the ring-3 path"
```

---

## Task 12: struct stat ABI — zgranie LinuxStat z picolibc x86_64 (§4 pkt 9)

**Files:** Modify `user/libc-glue/syscalls.c` (mapowanie), nota w `kernel/Syscall.h`

> To wymuszone przez ABI (§4 pkt9, §8). Kernelowy `LinuxStat` jest kompaktowy i NIE jest
> on-disk ABI — userland mapuje go na picolibc `struct stat`. Na x86_64 picolibc `st_ino`/
> `st_size` są 64-bit, a `time_t` 8-bajtowy. `Syscalls::stat` wypełnia `LinuxStat` polami
> `unsigned` (32-bit) — to wystarcza dla obecnych plików (< 4 GiB), ale mapowanie w
> userlandzie MUSI rozszerzać do 64-bit pól picolibc. Pełne 64-bit `st_size` w kernelu =
> Plan 7.

- [ ] **Step 1: Zaktualizować `fillstat`/`knl_stat` dla x86_64**

W `user/libc-glue/syscalls.c` `struct knl_stat` odpowiada kernelowemu `LinuxStat`
(7×`unsigned`, ta sama na obu arch). Mapowanie do `struct stat` jest już polowe
(`o->st_size = k->size` itd.) — picolibc x86_64 ma szersze pola, a przypisanie `unsigned`→
64-bit pole rozszerza się poprawnie. **Jedyna realna pułapka:** layout `struct timespec` w
`KTimespec` (kernel) musi == picolibc x86_64 (`time_t` 8B @0, `long tv_nsec` 8B @8 = 16B,
inaczej niż i686 12B). Dodaj w `kernel/Syscall.h` przy `KTimespec` notę + warunkowy layout:

```cpp
// Kernel mirror of picolibc struct timespec. i386: {long long sec @0; int nsec @8} = 12B.
// x86_64: {long long sec @0; long nsec @8} = 16B. Layout MUST match picolibc per arch.
struct KTimespec {
	long long tv_sec;
#if defined(__x86_64__)
	long tv_nsec;        // 8 bytes on LP64 (16-byte struct, picolibc x86_64)
#else
	int  tv_nsec;        // 4 bytes on i386 (12-byte struct)
#endif
};
```

- [ ] **Step 2: Host-test bez regresji**

```bash
make test
```
Expected: zielone (host arm64 LP64 — `KTimespec` 16B; clockGettime test, jeśli istnieje,
zgodny).

- [ ] **Step 3: Commit**

```bash
git add kernel/Syscall.h user/libc-glue/syscalls.c
git commit -m "abi: KTimespec/struct stat mapping matches picolibc x86_64 layout"
```

---

## Task 13: Boot do ring 3 — headless QEMU (główna bramka)

**Files:** (brak nowych — boot obrazem z Planu 5)

- [ ] **Step 1: Zbudować obraz z 64-bitowym init**

```bash
make ARCH=x86_64 image      # _all (kernel) + _userland64 path writes /nanos/core/init.nxe
```
Expected: `disk/image-grub2.img` z 64-bitowym `kernel.bin` + `init.nxe` (v4). Ustaw w
`grub.cfg` `timeout=0` na czas testu (przywróć po).

- [ ] **Step 2: Boot interaktywny — zobaczyć wyjście init**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file=disk/image-grub2.img,format=raw
```
Expected: w konsoli VGA: `NanOS x86_64 userland: ring 3 + syscall OK`, a pod spodem treść
`grub.cfg`. Maszyna kończy spokojnie (init `exit(0)` → kernel idle).

- [ ] **Step 3: Weryfikacja headless + brak faultów (wzorzec z CLAUDE.md)**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file=disk/image-grub2.img,format=raw \
  -display none -monitor unix:/tmp/qmon64,server,nowait -no-reboot -d int -D /tmp/qlog64 &
QEMU_PID=$!
sleep 3
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon64"); s.recv(4096)
s.sendall(b"screendump /tmp/ring3.ppm\n"); import time; time.sleep(1)
PY
kill $QEMU_PID 2>/dev/null
sips -s format png /tmp/ring3.ppm --out /tmp/ring3.png >/dev/null
grep -E "v=0d|v=08|v=0e" /tmp/qlog64 | head      # GP / triple / #PF — MUST be empty
echo "Open /tmp/ring3.png — expect the OK line + grub.cfg, ring 3."
```
Expected: PNG pokazuje komunikat init + `grub.cfg`; `grep` faultów PUSTY (0 #GP/#PF/triple).
Jeśli `v=0e` (page fault) z `cr2` małym → `%fs.base`/TLS dotknięty mimo braku TLS (sprawdź,
że minimal init nie linkuje picolibc); `v=0d` → zła ramka `iretq`/selektory (Task 0 GDT).

- [ ] **Step 4: Przywrócić grub.cfg timeout + commit**

```bash
# restore grub.cfg timeout=5
git add grub.cfg
git commit -m "x86_64: boot to ring 3 — init.nxe makes syscalls via SYSCALL, 0 faults headless" --allow-empty
```

---

## Task 14: Domknięcie planu

- [ ] **Step 1: Brak regresji i686**

```bash
docker run --rm -v $(pwd):/src -w /src nanos-build sh -c 'make build && make check-arch'
make test
```
Expected: i686 kernel buduje się; `OK: MI layer is arch-clean.`; host-testy zielone
(NxeLoader v4, Syscalls dispatch).

- [ ] **Step 2: Zaktualizować spec — odhaczyć kamień 6**

W `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`, §7, dopisać przy
kamieniu 6 adnotację „✅ zrealizowane w plans/2026-06-15-x86_64-plan-6-syscalls-userland.md".

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md
git commit -m "docs: mark x86_64 milestone 6 (syscalls + userland + .nxe v4) done"
```

---

## Self-Review

- **Spec coverage:** realizuje kamień 6 z §7. Decyzja #1 (model small/niska baza/
  `R_X86_64_64`+`R_X86_64_32S`, `NxReloc.off` 64-bit) — Tasks 3–6, 10. Decyzja #2
  (`%fs.base`) — Task 9 (`archSetUserFsBase`, haczyk `arch_prctl`, nota o ustawianiu przy
  context-switch). Decyzja #3 (SSE asymetria) — Task 10 `USER64_CFLAGS` (SSE ON) kontra
  kernel `KARCHFLAGS` (`-mno-sse`, Plan 1). ABI numerów x86_64 — Task 1. `syscall/sysret`
  + MSR — Tasks 7–8. crt0 SysV + wrappery — Task 10. `struct stat`/`KTimespec` ABI — Task 12.
- **Placeholders:** brak — każdy krok ma realny kod (NASM/C/C++) + komendę + oczekiwany
  wynik. Minimalny `init.nxe` świadomie BEZ picolibc/`libc.ndl`/`nsh` (to Plan 8) —
  kryterium wyjścia „cat grub.cfg + exit w ringu 3" jest jawnie dopuszczone przez prompt.
- **i686 zielony:** każda zmiana współdzielona jest warunkowa (`__x86_64__`/`NX_FORCE64`)
  lub szerokościowo-neutralna (`unsigned long`/`uintptr_t`/`nxaddr_t` = 32-bit na i686).
  Regresja sprawdzana `make build && make check-arch` (Task 14) — zgodnie z filozofią
  Planu 1 „oba zielone w okresie przejściowym".
- **TDD na hoście:** `NxeLoader` (Task 4: test→czerwony→implementacja→zielony) i `mknx64`
  (Task 5 smoke na realnym ELF64). `Syscalls`/dispatch — `make test` po poszerzeniu args.
- **Twarde zależności layoutu:** kolejność GPR w `syscall_entry64.S` i pola
  `kernel::Registers` (Plan 4) — jawnie oznaczone (Task 0/7/8/9) jako punkt synchronizacji.
  Układ GDT (Plan 4) wymuszony przez `SYSRET`/`STAR` — podany w Task 0 z wartością `STAR`.
- **Naming contract:** `arch/x86_64/cpu/`, toolchain `x86_64-elf-`, kolizje obiektów z
  sufiksem `64` (`nxjmp64.o`, `syscall_entry64.o`, `user/*64.o`, `mknx64`). `.nxe` = v4,
  loader odrzuca v3 (test w Task 4). Boot przez obraz GRUB (Plan 5).

## Zależności

- **Plan 2** (MI `kmain`/`Console` na x86_64) — `init` pisze przez kernelową konsolę.
- **Plan 3** (paging 4-poziomowy) — `AddressSpace`/`mmuMap`/`mmuSwitch`/`g_frames`/
  `mmuUserHeapBase`.
- **Plan 4** (GDT/IDT/TSS 64-bit + `kernel::Registers` + `iretq`/`swapgs` + `setKernelStack`)
  — **najtwardsza zależność**: layout `Registers` i selektory GDT (Task 0).
- **Plan 5** (boot z ext4) — odczyt `/disks/main/boot/grub/grub.cfg`.
- **Nakłada się z Planem 7** (LP64): tu robimy MINIMALNY podzbiór (`kernelSyscall` args,
  `Exec` entry, `NxFormat`/`NxeLoader` adresy, `usermode.h` adresy, `KTimespec`); pełny
  sweep `-Wconversion` (FrameAllocator, `Console::writeHex`, `Fd`/`lseek` `off_t`,
  `LinuxStat.st_size` 64-bit) — Plan 7.
- **Poprzedza Plan 8** (SDK `x86_64-nanos` + picolibc + `libc.ndl` + porty): dopiero tam
  `init.nxe` przechodzi na picolibc i osiąga `nsh`.
