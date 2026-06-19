# Analiza migracji NanOS: i686 (32-bit) → x86_64 (long mode)

> Dokument analityczny. Stan repo: gałąź `feat/nap-package-manager`, 2026-06-15.
> Nie jest to plan wykonawczy — to inwentaryzacja powierzchni zmian, decyzji
> projektowych i oszacowanie wysiłku. Plan TDD powstanie osobno.

---

## 0. Streszczenie kierownicze (TL;DR)

- **Architektura kodu jest gotowa na port.** Podział MI/MD (`arch/include/arch/` jako
  kontrakty, `arch/x86/` jako implementacja, przełącznik `ARCH ?= x86` →
  `arch/$(ARCH)/arch.mk`) to dokładnie ten mechanizm, który port czyni wykonalnym.
  Dodanie x86_64 = nowy katalog `arch/x86_64/` + `x86_64-elf` toolchain, bez dotykania
  rdzenia w idealnym przypadku.
- **MI-kod jest już dziś kompilowany i testowany jako LP64.** `docker/Dockerfile.test`
  jest natywnie-architekturalny — na macOS/Apple Silicon testy host-owe budują się jako
  64-bit (ARM64, LP64, 8-bajtowe wskaźniki). To znaczy, że duża część kodu MI **już
  przechodzi przez 64-bitowy kompilator i test suite**. Pułapki LP64 wymienione w §4 to
  realne ryzyka, ale nie jest to dziewiczy teren.
- **Najtańsze części:** assembler (~540 linii łącznie), struktury GDT/IDT/TSS, ścieżka
  przerwań. To dużo przepisywania *na linię*, ale to kod ograniczony i dobrze zrozumiały.
- **Najdroższa część to NIE kernel.** To **cały ekosystem `nanos-sdk` i wszystkie
  zportowane aplikacje** (bash, OpenSSL, vim, GNU grep, Doom, Dropbear, NetSurf,
  inetutils/ping, busybox, darkhttpd…). Każda wymaga nowego toolchaina `x86_64-nanos`
  i pełnej przebudowy. To dominujący koszt projektu.
- **Format `.nxe`/`.ndl` wymaga wersji v4** z 64-bitowymi adresami i relokacjami
  `R_X86_64_64` zamiast `R_386_32`. Narzędzie `tools/mknx.c` i loader `NxeLoader.cpp`
  zmieniają semantykę wskaźników z 4 na 8 bajtów.
- **Boot:** GRUB i tak przekazuje sterowanie w 32-bitowym trybie chronionym. Kernel
  musi sam zbudować tablice stron (PML4, 4 poziomy), włączyć PAE + EFER.LME + paging
  i skoczyć w long mode. To nowy trampolina w `loader.s`, nie wymiana bootloadera.

**Rekomendacja:** port pełny 64-bit (kernel + userland w long mode natywnie), **nie**
tryb zgodności (32-bit userland pod 64-bit kernelem). Tryb compat oszczędza userland,
ale wymaga utrzymywania dwóch ABI syscalli i deskryptorów segmentów compat — większa
złożoność niż przebudowa aplikacji nowym toolchainem.

### 0.1. Założenie kluczowe: brak wsparcia dla CPU bez x86_64

**Nie musimy wspierać starych procesorów (pre-x64).** To nie jest detal — to przesądza
o całej strategii: **to migracja/zastąpienie, a nie dodanie równoległej architektury.**
Konsekwencje (rozwinięte w §1.1):

- **Brak utrzymywania dwóch architektur.** i686 może przestać się budować w momencie
  cut-over. Nie ma macierzy CI 32/64, nie ma kompatybilności wstecznej do pilnowania.
- **Swobodna zmiana wspólnych sygnatur MI** (np. `kernelSyscall` na `uintptr_t`,
  kontrakty `arch/*` na 64-bit) — bez troski o ILP32. Commit-ujemy się w LP64.
- **Jeden toolchain.** Po migracji znika `i686-elf` i `i686-nanos`; Docker buduje
  tylko `x86_64-elf` (mniejszy obraz, krótszy build). SDK staje się wyłącznie
  `x86_64-nanos`.
- **Format `.nxe` tylko v4.** Loader nie musi obsługiwać starego v3 (32-bit) — czyste
  cięcie. Stare binaria 32-bit po prostu nie działają (i tak wszystko przebudowujemy).
- **Wolno założyć bazową linię cech x86_64:** SSE2 jest gwarantowane (część baseline
  AMD64) → można zdjąć `-mno-sse`/wyłączony SIMD w Rust target, picolibc i aplikacje
  dostają szybszą matematykę; bit **NX** zawsze dostępny (W^X); instrukcja `syscall`
  zawsze obecna w long mode. (Wyjątek: `RDRAND` nie jest gwarantowane na najwcześniejszych
  x86_64 — zostawiamy fallback na TSC; na QEMU bez znaczenia.)
- **Koszt aplikacji bez zmian co do skali, ale jednorazowy** — przebudowa ~12 portów
  raz, nie utrzymywanie dwóch wariantów w nieskończoność.

---

## 1. Strategia i decyzje projektowe (do podjęcia przed kodem)

| Decyzja | Opcje | Rekomendacja |
|---|---|---|
| **Model portu** | (A) dodać `arch/x86_64` obok `arch/x86`, oba zielone; (B) **zastąpić** — zbudować x86_64, usunąć x86 po cut-over | **(B)** — wynika z §0.1; jeden cel, brak dual-arch |
| **Zakres bitowości** | (A) pełny 64-bit kernel+userland; (B) 64-bit kernel + 32-bit compat userland | **(A)** — czystsze, jedno ABI; koszt = rebuild aplikacji (i tak nieuchronny) |
| **Układ kernela w VA** | (A) low-half, identity (jak teraz, kernel @ 1 MiB); (B) higher-half (`0xFFFFFFFF80000000`) | **(B) docelowo**, ale można wystartować od (A) identity dla pierwszego boot-u i przenieść później. Higher-half to standard x86_64 i upraszcza separację user/kernel |
| **Instrukcja syscall** | (A) `int 0x80` (działa też w long mode); (B) `syscall`/`sysret` | **(B)** — kanoniczne dla x86_64, szybsze; `int 0x80` zostawić ewentualnie jako legacy |
| **Numery syscalli** | i386 vs x86_64 (zupełnie różne: `write` 4→1, `exit` 1→60, `read` 3→0…) | Przejść na **numery x86_64** (picolibc/aplikacje tego oczekują przy triplecie x86_64) |
| **Bootloader** | (A) Multiboot1 + trampolina long-mode w kernelu; (B) Multiboot2; (C) UEFI | **(A)** najmniejsza zmiana (GRUB i tak ładuje w PM32; sami wchodzimy w long mode). Multiboot1 nie adresuje >4 GiB w mmap — patrz §2.1 |
| **Stałe MI ↔ arch** | adresy okien VA (`0x800000`, `0x40000000`…) rozsiane w MD i MI | Skonsolidować w kontrakcie `arch/mmu.h`; MI nie powinno znać literałów adresowych |

### 1.1. Model „zastąpienia" w praktyce

Skoro nie utrzymujemy 32-bitu (§0.1), zalecana mechanika:

1. **Zachowujemy przełącznik `ARCH`** i podział MI/MD — są tanie i pozostają wartościowe
   pod ewentualny przyszły port ARM. Tworzymy `arch/x86_64/` jako *jedyną wspieraną*
   implementację kontraktów.
2. **`arch/x86/` zostaje tymczasowo** jako referencja podczas portu (czytamy z niego
   strukturę), ale **po pierwszym zielonym boocie x86_64 jest usuwany** — nie zostaje
   jako martwy, nietestowany kod.
3. **Wspólne sygnatury MI przechodzą od razu na 64-bit** (`uintptr_t`/`uint64_t`/`size_t`),
   bez warstw zgodności. Build 64-bit z `-Wconversion` + istniejący test suite host-owy
   są siatką bezpieczeństwa (§4).
4. **Toolchain i SDK:** dwa cele istnieją tylko w okresie przejściowym; docelowo Docker
   i `nanos-sdk` budują wyłącznie `x86_64-*`. Stary `i686-elf`/`i686-nanos` usuwamy z
   `docker/Dockerfile`, `arch.mk`, ścieżek SDK i specu Rust.

> Innymi słowy: §2–§5 opisują *co* przepisać; §0.1 mówi, że robimy to **w miejsce**
> 32-bitu, nie obok niego — co eliminuje większość kosztu „utrzymaniowego" portu
> i pozwala iść agresywniej (łamać ABI MI, ciąć stare formaty) zamiast budować mosty.

---

## 2. Warstwa maszynowo-zależna (`arch/x86/`) — co przepisać

Łącznie ~2900 linii w `arch/x86`, z czego ~540 to assembler. **To jest właściwa
robota portu kernela.** Tworzymy `arch/x86_64/` jako równoległą implementację tych
samych kontraktów z `arch/include/arch/`.

### 2.1. Boot (`arch/x86/boot/`)

- **`loader.s`** (82 linie, GNU as AT&T, 32-bit): nagłówek Multiboot1
  (`0x1BADB002`), `lgdt`/`lidt`, `movl $stack_top, %esp`, far-jump `$0x08,$flush`.
  - **Zmiana:** pozostaje 32-bitowym punktem wejścia (GRUB ładuje w PM32), ale po
    skonfigurowaniu stosu musi: zbudować tymczasowe tablice stron (PML4→PDPT→PD,
    identity-map pierwszych N MiB hugepage'ami 2 MiB), ustawić `CR4.PAE`,
    `EFER.LME` (MSR `0xC0000080`, bit 8), `CR0.PG|PE`, załadować 64-bitowy GDT
    z deskryptorem kodu z bitem `L=1`, i far-jump do 64-bitowego punktu wejścia.
  - To nowa **trampolina ~80–120 linii asm**. Klasyczny wzorzec OSDev „32→64".
- **`MultibootInfo.h` / `MultibootMmap`**: `mmap_addr` @ off 48 jest `uint32_t`;
  `framebuffer_addr` jest już `uint64_t`. Multiboot1 nie reprezentuje regionów
  pamięci >4 GiB. Dla maszyn QEMU ≤4 GiB to nie problem dziś, ale to dług. Jeśli
  kiedyś chcemy >4 GiB RAM → Multiboot2.

### 2.2. CPU / przerwania (`arch/x86/cpu/`) — najgęstsze zmiany

| Plik | Co jest 32-bit-bound | Zmiana na x86_64 |
|---|---|---|
| `Gdt.{h,cpp}` | `GdtEntry` 8 bajtów, baza 24-bit, bit D/B; 6 wpisów | Wpisy danych/kodu trywialne (long mode ignoruje bazę/limit, liczy się bit `L`); **wpis TSS staje się 16-bajtowy** (baza 64-bit) |
| `Idt.{h,cpp}` | `IdtEntry` 8 bajtów, baza 32-bit, `always0` | **`IdtEntry` 16 bajtów**: baza 64-bit (`base_lo/mid/hi`), pole **IST** (3 bity), rezerwa. 256 wektorów bez zmian |
| `Tss.h` | 104 B, `esp0/ss0`, wszystkie GPR/segmenty | **64-bit TSS 136 B**: `rsp0/rsp1/rsp2`, `ist1..ist7` (po 8 B), `iomap_base`; brak `ss0`, brak zapisanych GPR |
| `Interrupt.h` (`Registers` = TrapFrame) | 19×`unsigned` (76 B): `ds`, `edi…eax` (pusha), `int_no/err`, `eip/cs/eflags/useresp/ss` | Nowy `Registers`: `rax…r15` (16×8 B), `rip/cs/rflags/rsp/ss`, `int_no/err`. **Brak `pusha/popa` w 64-bit** — push/pop ręcznie 15 rejestrów |
| `isr.S` (89) | `pusha`/`popa`, `add esp,8`, `iret`, sel 0x10; `isr128` `push dword 128` | Ręczny zrzut `push rax…r15`, `swapgs` przy wejściu z ringu 3, `iretq`. Uwaga **red zone (128 B)** — handler nie może na nim deptać |
| `irq.S` (64) | `pushad`, `test dword [esp+48],3` (CPL), `schedPreempt()` | Analogicznie ręczny zrzut; offset CPL inny (cs na innej pozycji); `iretq` |
| `switch.S` (48) — `archContextSwitch` | zapisuje `ebx/esi/edi/ebp`+cr3 (cdecl, args na stosie) | Zapis **callee-saved System V**: `rbx, rbp, r12–r15` (+cr3); **args w `rdi/rsi`**, nie na stosie |
| `nxjmp.S` (32) — setjmp/longjmp dla `exit()` | `NxJmp{esp,ebp,ebx,esi,edi,eip}` 24 B | `{rsp,rbp,rbx,r12–r15,rip}` 64 B; ret-val w `rax` |
| `syscall_x86.cpp` (63) | `int 0x80`, args `r->eax/ebx/ecx/edx/esi/edi/ebp` | Wejście przez `syscall` (MSR `LSTAR`/`STAR`/`FMASK` w init); args `rdi/rsi/rdx/r10/r8/r9`, nr w `rax`, ret w `rax`; `rcx`=zapisany RIP, `r11`=RFLAGS |
| `usermode_x86.cpp` (182) | selektory `0x23/0x1B`, `iret` do ring 3; `SigContext` 44 B; `eip-=2` restart syscalla | `iretq` lub `sysret`; ramka sygnału ~2× większa (wszystkie 64-bit GPR); restart cofa o długość `syscall` (2 B) |
| `cpu_x86.cpp` | `pushf/pop eax/cli`, `cpuid`, ACPI poweroff | EFLAGS→RFLAGS (`pushfq`); CPUID bez zmian (nazwy rej.) |
| `random_x86.cpp` (59) | `archEntropyTick()` bierze tylko **dolne 32 bity** RDTSC; RDRAND 32-bit | RDRAND 64-bit, zwracać pełne 64 bity TSC |
| `sched_x86.cpp`, `fault_x86.cpp`, `fork_x86.cpp` | `r->cs & 3` (CPL), `readCr2()`, `ret_from_fork` | offsety w TrapFrame; `CR2` 64-bit; logika bez zmian |

### 2.3. Pamięć / paging (`arch/x86/mm/`)

To koncepcyjnie największa zmiana struktur danych.

- **`Paging.h`** — dziś 2-poziomowy podział VA: `[PD(10)|PT(10)|off(12)]`, maska
  `0xFFFFF000`, 1024+1024 wpisów. **x86_64 = 4 poziomy**:
  `[PML4(9)|PDPT(9)|PD(9)|PT(9)|off(12)]`, po 512 wpisów, wpisy 8-bajtowe, bit **NX**
  (63) dostępny. Wymaga obsługi adresów kanonicznych (znak-rozszerzenie bitu 47).
- **`PagingControl.h`** — `loadCr3/readCr3/readCr2/enablePaging` na `uint32_t`.
  → `uint64_t`; doszłaby konfiguracja `EFER.NXE` dla bitu NX.
- **`AddressSpace.{h,cpp}`** (host-testowane, 194 linie) — `PagingEnv` z
  `uint32_t (*allocFrame)`, `map(uint32_t va, pa, flags)`, 1024 PDE. → 64-bit
  adresy, 512 wpisów/poziom, rekurencyjne schodzenie 4 poziomów. **Plus:** ten plik
  ma testy host-owe — port da się rozwijać TDD na hoście.
- **`mmu_x86.cpp`** (268) — identity-map całego RAM, stałe okna VA:
  user `0x800000–0x1000000`, biblioteki `0x40000000`, heap `0x48000000`,
  mmap `0x50000000`, FB `0x58000000`. **W 64-bit te okna należy przeprojektować**
  (przestrzeń 128 TiB user). Decyzja: zachować podobne offsety low-canonical na
  start (minimalna zmiana) czy rozsunąć w pełną przestrzeń (docelowo).

### 2.4. Sterowniki (`arch/x86/drivers/`, `arch/x86/io/`)

- **`ATA.S`** (221, NASM): port I/O `out dx,al`/`in al,dx`, `rep insw`. Port I/O
  działa identycznie w long mode — zmiany kosmetyczne (rejestry adresowe `edi`→`rdi`
  dla bufora). **Najmniej dotknięty plik asm.**
- **`console_x86.cpp`**: VGA `0xB8000` — adres fizyczny, w identity/lo-map bez zmian.
- **`pci_x86.cpp`** (56), `IOPort.{h,cpp}`: mechanizm CONFIG `0xCF8/0xCFC` bez zmian;
  uwaga na 64-bitowe BAR-y w kodzie MI (patrz §4).

### 2.5. Kontrakty `arch/include/arch/` do rozszerzenia

Wszystkie sygnatury operujące na adresach/SP/CR3 jako `uint32_t`/`unsigned` muszą
dostać szerszy typ (`uint64_t`/`uintptr_t`). Dotyczy: `mmu.h`
(`mmuInitKernel`, `mmuMap`, `mmuKernelDirPhys`), `usermode.h`
(`archLoadUser/archEnterUser/archPushSignalFrame`), `sched.h`
(`archContextSwitch/archTaskBootstrap/setKernelStack`), `bootinfo.h` (`bootMemTop`),
`random.h`, `cpu.h` (`cpuIrqSave`→ RFLAGS). **Te zmiany sygnatur dotykają też MI**,
bo MI woła te kontrakty — najlepiej od razu na `uintptr_t`.

---

## 3. Build system i toolchain

Brak jawnego `-m32` — bitowość pochodzi z **domyślnej triplety toolchaina**
(`i686-elf`). To upraszcza port: dodajemy równoległy `x86_64-elf`, nie walczymy z flagami.

### 3.1. Co dodać / sparametryzować

| Miejsce | Stan 32-bit | Zmiana |
|---|---|---|
| `Makefile:3` | `ARCH ?= x86` | nowy `arch/x86_64/arch.mk` wybierany przez `ARCH=x86_64` |
| `arch/x86/arch.mk:1` | `CROSS ?= i686-elf-` | `x86_64-elf-` w nowym arch.mk; nowa lista `ARCH_SOURCES` (loader64, switch64…) |
| `Makefile` asm | `nasm -f elf` | `nasm -f elf64` |
| `arch/x86/linker.ld` | `.text 0x100000`, brak OUTPUT_FORMAT (domyśl. elf32) | `OUTPUT_FORMAT(elf64-x86-64)`, układ higher-half/`KERNEL_VMA` |
| `Makefile` run | `qemu-system-i386` | `qemu-system-x86_64` |
| `user/nx.ld:11` | `. = 0x800000` | baza user 64-bit; `__nx_image_size` arytmetyka 64-bit |
| `user/dll.ld:8` | baza DLL `0x09000000` | adres 64-bit |
| `user/rust/i686-nanos.json` | `target-pointer-width: 32`, `i686-elf-ld` | nowy `x86_64-nanos.json` (`width:64`, data-layout p:64, `x86_64-elf-ld`) |

### 3.2. Docker (`docker/Dockerfile`)

- Toolchain budowany ze źródeł: `TARGET=i686-elf`, binutils 2.43, gcc 14.2.0,
  picolibc 1.8.6 (`-Dprefix=/opt/picolibc/i686-elf`), cross-file
  `picolibc-i686-elf.txt` (`cpu='i686'`).
  - **Zmiana:** drugi build z `TARGET=x86_64-elf` + `picolibc-x86_64-elf.txt`
    (`cpu='x86_64'`, binaria `x86_64-elf-*`). To kolejne ~20–40 min pierwszego
    builda (emulacja amd64 na Apple Silicon) — można cache'ować w osobnej warstwie.
- **GRUB:** dziś `i386-pc` (BIOS, `boot.img` 440 B, `grub-mkimage -O i386-pc`).
  Long-mode kernel ładowany Multiboot1 działa **z tym samym `i386-pc` GRUB-em** —
  GRUB startuje w PM32, my wchodzimy w long mode. **Nie trzeba** EFI GRUB, dopóki
  trzymamy Multiboot1 + trampolina. (`scripts/create-grub2-image.sh` bez zmian.)
- `docker/Dockerfile.test`: natywny `g++`, bez crossa — **już buduje testy jako
  LP64** na amd64/arm64. Zostaje bez zmian; staje się naszym pierwszym sitem na błędy
  64-bit w MI.

---

## 4. MI-kod: pułapki LP64 (8-bajtowy wskaźnik)

MI jest *w większości* przenośne (i już testowane na LP64), ale audyt wykrył realne
miejsca, gdzie 32-bitowość jest wprost zakodowana. **Kluczowe:** te błędy nie
ujawniają się dziś, bo kernel jest ILP32, a testy host-owe rzadko przepychają adresy
>4 GiB. Na 64-bit kernelu zaczną gubić górne bity.

### Krytyczne (urwą się na 64-bit)

1. **`mm/FrameAllocator.h`** — cały alokator zaszyty na 32-bit: `MAX_FRAMES = 1<<20`
   (4 GiB limit), bitmapa 128 KiB, `alloc()→uint32_t`, `init(uint32_t topOfRam)`.
   Wymaga redesignu na `uint64_t` adresy fizyczne (albo świadomy limit).
2. **`mm/memory_manager.cpp`** — `malloc/calloc/realloc` rzutują `size` na
   `(unsigned)` (obcięcie >4 GiB); `heapPutHex` pętla `i=28;i>=0;i-=4` drukuje tylko
   32 bity. `heapInit(void*, unsigned size)`.
3. **`kernel/SyscallDispatch.cpp:245`** — `kernelSyscall(int nr, unsigned a0..a5, …)`:
   **argumenty syscalli jako `unsigned`** → obcięcie 64-bitowych wskaźników/rozmiarów.
   Liczne `(void*)A[i]` gubiłyby górne bity. To centralny punkt ABI — musi przejść na
   `uintptr_t`/`unsigned long`.
4. **`kernel/Kernel.cpp`** — `markFree(...,(uint32_t)base,(uint32_t)len)` (l. 53),
   `mmuMapKernelMmio((uint32_t)fb->addr)` (201), `FbInfo{(uint32_t)fbdev->addr}` (303),
   `writeHex((int)nic.bar[0].addr)` (240): **rzutowania adresów MMIO/BAR/FB na 32-bit**.
5. **`drivers/Console.{h,cpp}`** — `writeHex(int)`, `itoa(int,int)`: nie umieją
   wydrukować 64-bitowego adresu. Potrzebne przeciążenia `uint64_t`/`unsigned long`.
6. **`drivers/Framebuffer.cpp`** — arytmetyka offsetu z `(uint32_t)y * pitch` może
   przepełnić przy dużych FB.
7. **`kernel/Exec.cpp`** — `STAGE_BASE/STAGE_CAP` jako `unsigned`; `(char*)STAGE_BASE`,
   `entry`/`esp` jako `unsigned`. → `uintptr_t` lub przez kontrakt `arch/mmu.h`.
8. **`kernel/Syscall.h`** — `struct Fd { unsigned offset; unsigned size; }` i
   `lseek(int,int,…)`: pliki/offsety ograniczone do 4 GiB / `int`. Na 64-bit
   należałoby `off_t` (64-bit). (To też dług funkcjonalny niezależny od portu.)
9. **`kernel/Syscall.h` `struct LinuxStat`** — `st_size` jako `unsigned`. ABI x86_64
   Linuksa ma 64-bitowy `st_size`/`st_ino`; picolibc x86_64 oczekuje innego layoutu
   `struct stat`. Wymaga zgrania z definicją picolibc dla x86_64-nanos.

### Istotne (czystość typów / ryzyko obcięcia)

- `lib/String.h` (`int length`), `lib/List.h` (`int capacity/count`) — rozmiary jako
  `int`; powinny być `size_t`. Działa, ale niespójne na LP64.
- `mm/Heap.{h,cpp}` — `size` jako `unsigned`; `NIL=0xFFFFFFFFu` (OK jako offset wewn.).
- `fs/Vfs.h` — `mmapInfo(String, unsigned*, unsigned*)`: `physOut` powinien być
  `uint64_t*`/`uintptr_t*` (adres fizyczny).
- `drivers/BlockDevice.h` — `readSectors(unsigned lba,…)`: `unsigned` dwuznaczne;
  jawnie `uint32_t`/`uint64_t`.
- `fs/ExtFilesystem.h` / `Ext4Filesystem.h` — pola superbloku/deskryptorów jako `int`,
  liczne `*(unsigned*)(buf+off)`. **Struktury on-disk są stałoszerokościowe (dobrze)**,
  ale in-memory typy i ext4 48-bitowe numery bloków warto ujednolicić.

**Wniosek:** to praca głównie mechaniczna (`unsigned`→`uintptr_t`/`size_t`/`uint64_t`
w wyznaczonych miejscach), wykrywalna kompilatorem z `-Wconversion` na buildzie 64-bit
i wyłapywana przez istniejący test suite host-owy. Granica MI/MD (syscall dispatch,
kontrakty arch) to miejsca o najwyższym priorytecie.

---

## 5. Userland, ABI i format `.nxe`

### 5.1. ABI syscalli (user → kernel)

- **`user/crt0.S`** (`[BITS 32]`): `_start` czyta `argc/argv/envp` ze stosu (cdecl),
  pushuje args. **Pełne przepisanie** na System V AMD64: argc/argv ze stosu wg ABI
  x86_64 (`rsp`→argc, potem argv[]), args do `main` w `rdi/rsi/rdx`, wyrównanie stosu
  16 B, `__nx_set_environ`.
- **`user/libnanos.c`, `user/libc-glue/syscalls.c`** — `sys3(...){ int $0x80; "b/c/d" }`.
  → warianty `syscall` z `"D"(a0),"S"(a1),"d"(a2),"r10"(a3),"r8"(a4),"r9"(a5)`,
  klobry `rcx,r11`. Typy `int`→`long`.
- **`user/sigtramp.S`** — `mov eax,119; int 0x80` (sigreturn i386). → numer x86_64
  sigreturn + `syscall`.
- **`kernel/SyscallNr.h`** — 109 numerów wg **i386** (`exit=1,read=3,write=4,…`,
  `mmap2=192`, `getdents64=220`, socket-y bezpośrednie `359+`, custom `termmode=501`).
  Migracja na numery **x86_64** (różne dla wszystkich: `read=0,write=1,open=2,…,
  exit=60`, `mmap=9`). Plik współdzielony z SDK (`cp kernel/SyscallNr.h →
  i686-nanos/include`), więc zmiana propaguje się do aplikacji.

### 5.2. Format wykonywalny `.nxe`/`.ndl`

- **`kernel/NxFormat.h`** — `NxHeader` i deskryptory (`NxImport{nameOff,slotAddr,libOff}`,
  `NxExport{nameOff,addr}`, `NxReloc{off}`) używają `unsigned` (4 B) dla **wszystkich
  adresów absolutnych**. → **format v4**: pola adresowe `uint64_t`, gniazda IAT
  64-bitowe.
- **`kernel/NxeLoader.cpp`** — relokacje `*(unsigned*)(img+off)+=delta`, wiązanie
  importów `*(void**)(img+slot)=addr`. → zapisy 64-bitowe; typ relokacji
  `R_386_32`→`R_X86_64_64`.
- **`tools/mknx.c`** — waliduje `e_machine==EM_386`, przetwarza `R_386_32/R_386_PC32`,
  używa makr `ELF32_*`. → akceptować `EM_X86_64` (62), relokacje `R_X86_64_64`(1),
  `R_X86_64_PC32`(2), `R_X86_64_32S`(11), makra `ELF64_*`.
- **Bazy ładowania:** exec `0x800000`, DLL `0x09000000`, pasmo modułów
  `0x40000000` (32×4 MiB) — przeprojektować w 64-bit (patrz §2.3).

### 5.3. Ekosystem aplikacji i SDK — **dominujący koszt, osobny strumień prac**

To jest *jawnie objęte* planem, ale jako **oddzielny workstream**, nie część kamieni
kernelowych. Dwie rzeczy są tu kluczowe i wynikają wprost ze struktury repo:

**(a) Decoupling jest wbudowany.** `make image` **nigdy nie zależy** od aplikacji —
brakujący artefakt nie psuje buildu kernela (`Makefile` l. 63, 67, 108…). SDK żyje w
osobnym repo `~/Projects/nanos-sdk` + work-dir `~/Projects/nanos-sdk-work`. Dzięki temu
kernel można doprowadzić do 64-bit *zanim* tknie się którąkolwiek aplikację.

**(b) Powierzchnia kontraktu jest mała i jawna.** Każdy target portu kopiuje do sysrootu
toolchaina dokładnie 4 rzeczy (`Makefile` l. 95–98 i analogiczne):
```
user/libc-glue/include/.  →  <triple>/include/
kernel/SyscallNr.h        →  <triple>/include/SyscallNr.h
bin/libc.ndl.a            →  <triple>/lib/libc.a
bin/libc.ndl              →  <triple>/lib/libc.ndl
```
To znaczy: **gdy te cztery artefakty staną się 64-bitowe** (nowe numery syscalli x86_64,
64-bitowy `libc.ndl`, nagłówki LP64), wszystkie porty przebudują się względem nich. To
jest dźwignia całej migracji userlandu.

**Co plan musi objąć — w kolejności zależności:**

1. **In-tree userland najpierw** (`user/`: `crt0.S`, `libnanos.c`, `libc-glue/`,
   `libc.ndl`). To buduje **toolchain kernela** (`i686-elf`→`x86_64-elf`), nie SDK, i
   produkuje `libc.ndl` — fundament, przeciw któremu linkują się *wszystkie* aplikacje.
   Dopóki to nie jest 64-bit, nic dalej nie ruszy. (Patrz §5.1, §5.2.)

2. **Toolchain `x86_64-nanos`** w `nanos-sdk`. Budowany ze źródeł przez
   `build-toolchain.sh` (`TARGET=i686-nanos`, binutils+gcc, instalka do `/opt/...`).
   Custom triplet `nanos` jest wstrzykiwany przez `toolchain/patch.sh`, który:
   - dodaje `nanos*` do `config.sub` — **już OS-agnostyczny, działa dla x86_64 bez zmian**;
   - rejestruje `i[3-7]86-*-nanos*` w `bfd/config.bfd`, `ld/configure.tgt`,
     `gas/configure.tgt` — **trzeba dodać bliźniacze reguły `x86_64-*-nanos*`** (mirror
     `x86_64-*-elf`);
   - dodaje tuple `i[34567]86-*-nanos*` do `gcc/config.gcc` z `tm_file=… i386/nanos.h` —
     **trzeba dodać `x86_64-*-nanos*`** z bazowymi nagłówkami x86_64 (`i386/x86-64.h` itd.);
   - instaluje `gcc/config/i386/nanos.h` (definiuje `__nanos__`, `STARTFILE_SPEC=
     "crt0.o nxhdr.o"`, `LINK_SPEC="-T nx.ld --emit-relocs"`, komentarz „keep R_386_32
     for mknx"). **Wariant 64-bit** musi trzymać `R_X86_64_64` zamiast `R_386_32`
     (spójnie z §5.2 i `mknx`), a `crt0.o`/`nxhdr.o` są teraz obiektami 64-bit;
   - dodaje tuple w `libgcc/config.host` — bliźniacza reguła x86_64.

   To bounded, dobrze zrozumiana robota: **odwzorowanie istniejącego patcha i686 na
   x86_64**. Plus rebuild picolibc dla `x86_64-nanos` (picolibc wspiera x86_64).

3. **Rebuild każdego portu** — aplikacje to zewnętrzne „porty" (`<app>-port/nxport.toml`),
   budowane przez pythonowy driver `port/nanos-port` w kontenerze `nanos-sdk-dev`. Każdy
   to niezależny mini-port; przebudowują się **raz** względem nowego sysrootu. Lista:

   > bash (`bash-nanos`), OpenSSL (`openssl-port`), Dropbear (`dropbear-port`),
   > inetutils: ping/wget/telnetd/inetd/ifconfig/traceroute (`inetutils-port` +
   > `-services-port`), darkhttpd (`darkhttpd-port`), busybox/udhcpc (`busybox-1.36.1`),
   > NetSurf (`netsurf-nanos` + backend `libnsfb`/NanWM), GNU grep (3.11), vim (~3.7 MiB),
   > bzip2 (1.0.8).

   Ryzyko per-aplikacja jest **niejednorodne**:
   - większość = czysta rekompilacja (autotools/meson same wykryją LP64);
   - **OpenSSL** — ma własne ścieżki asm x86_64 (raczej plus); trzeba wskazać target
     `linux-x86_64`-podobny w jego `Configure` zamiast 32-bit;
   - kod z **inline-asm i386** lub założeniami `sizeof(long)==4`/`sizeof(void*)==4`
     zachowa się inaczej — wymaga audytu;
   - **NetSurf/libnsfb** — backend graficzny NanWM; rekompilacja, ale warto zweryfikować
     arytmetykę offsetów framebuffera (analogicznie do §4 pkt 6);
   - **struct stat / ABI** — zgranie z definicją picolibc x86_64 (§4 pkt 9).

To **nie jest praca kernelowa** — to *N równoległych mini-portów* po jednorazowym
ufundowaniu toolchaina. **Realnie większość budżetu kalendarzowego** całego
przedsięwzięcia, ale ryzyko jest rozproszone i każdy port jest niezależnie weryfikowalny.

> **Kluczowa zależność:** kroki 1→2→3 są sekwencyjne (libc.ndl → toolchain → apps), ale
> krok 3 jest wewnętrznie równoległy i całkowicie odseparowany od kernela. Apps można
> portować długo po tym, jak kernel x86_64 już bootuje i odpala `init.nxe`.

### 5.4. Czy toolchain i apki przeniosą się PROSTO? Przeszkody

Krótko: **toolchain — w większości tak, z jednym realnym wyjątkiem; apki — spektrum.**
Poniżej uczciwa lista, posortowana od najwyższego ryzyka.

**🔴 #1 (PRAWDZIWA przeszkoda) — model kodu + relokacje `.nxe`.** Cały schemat
`.nxe`/`mknx` opiera się na tym, że program jest linkowany **non-PIC pod stałą bazą**
(`nx.ld` @ `0x800000`) z `--emit-relocs`, a `mknx` zbiera relokacje **`R_386_32`**
(32-bit absolutne), które loader stosuje (`*(unsigned*)(img+off) += delta`). Na i386
to działa naturalnie. Na x86_64 zmienia się charakter relokacji:
- *kod* w modelu small staje się **RIP-relative** (`R_X86_64_PC32`) — bez relokacji,
  pozycyjnie niezależny, to akurat *plus*;
- *dane wskaźnikowe* (gniazda IAT, tablice wskaźników, zainicjalizowane pointery) →
  **`R_X86_64_64`** (8-bajtowe absolutne) — `mknx` musi je rejestrować, a loader robić
  `*(uint64_t*)(img+off) += delta64`;
- część konstrukcji w modelu small non-PIC emituje **`R_X86_64_32S`** (32-bit
  sign-extended) — te **wymuszają załadowanie obrazu w dolnych 2 GiB** przestrzeni.

**Konsekwencja / decyzja:** najprostsza ścieżka to **trzymać bazy ładowania user w
dolnych 2 GiB + model small (non-PIC)** i nauczyć `mknx` relokacji `R_X86_64_64`
(+ `R_X86_64_32S`), poszerzyć `NxReloc.off` do 64-bit. To **lekko zaprzecza** marzeniu
z §2.3 o pełnej 64-bitowej przestrzeni user — ale jest pragmatyczne i zgodne z obecną
filozofią „stała niska baza". Alternatywy (PIE/small-PIC z GOT albo model large z
`R_X86_64_64` wszędzie) to większy rework `mknx` i/lub wolniejszy kod. **To jest jedyna
decyzja w całym porcie, która nie jest mechanicznym odwzorowaniem** — reszta toolchaina
to mirror reguł i686→x86_64 (§5.3 pkt 2).

**🟠 #2 — integracja kernel ↔ picolibc przez `%fs` (thread pointer / TLS).** Na x86_64
SysV picolibc zwykle oczekuje **wskaźnika wątku w `%fs.base`** (errno, stdio, locale).
Na i386 model jest prostszy i NanOS go nie potrzebuje (`has-thread-local: false` w Rust
target). Na x86_64 kernel musi **ustawić `%fs.base` dla procesu user** (przez
`MSR_FS_BASE`/`wrfsbase` lub `arch_prctl(ARCH_SET_FS)`), inaczej pierwszy dostęp
picolibc do TLS-owego errno faultuje. To **nowa ścieżka integracyjna nieobecna w i386**
— realny snag, ale dobrze znany (każdy x86_64 OS to robi).

**🟠 #3 — asymetria SSE.** ABI x86_64 SysV **wymaga SSE** (floaty w XMM, varargs).
Userland/picolibc/apki **muszą** mieć SSE włączone (obecny Rust target wyłącza cały
SIMD — to trzeba zdjąć dla x86_64). Jednocześnie **kernel** musi `-mno-sse -mno-mmx
-mno-red-zone -mgeneral-regs-only` i gwarantować brak FP (albo zapisywać XMM przy
przełączaniu). Inaczej gcc wpleci SSE np. w `memcpy` i przy przerwaniu skorumpuje stan.
To konfiguracja flag, nie przepisywanie — ale łatwo o nią zapomnieć i dostać trudny bug.

**🟡 #4 — OpenSSL retargeting.** Per saldo *plus* (natywne, dobrze przetestowane ścieżki
asm x86_64), ale `nxport.toml` musi wskazać target `Configure` typu `linux-x86_64`
zamiast 32-bit; perlowe generatory asm muszą dostać właściwy flavor. Utarte, ale nie
zerowe.

**🟡 #5 — Doom i NetSurf/libnsfb.** Klasyczny Doom ma mnóstwo założeń `int==pointer`
(`fixed_t`, rzutowania) — jeśli to naiwny port, LP64 boli; nowoczesne porty (Chocolate)
są 64-bit-clean (zależy, którego użyto — do sprawdzenia). Backend `libnsfb` NanWM to
**nasz kod** z arytmetyką offsetów framebuffera → audyt jak §4 pkt 6.

**🟢 #6 — reszta apek przenosi się prosto.** bash, GNU grep, vim, bzip2, darkhttpd,
busybox, inetutils (ping/wget/…), Dropbear — dojrzały, przenośny C autotools/meson,
działający od lat na 64-bit Linuksie. Realnie **czysta rekompilacja** po ufundowaniu
toolchaina; LP64-czystość wykryją same skrypty configure.

**🟢 #7 — sam patch toolchaina jest bounded.** `config.sub` ma już `nanos*`
OS-agnostycznie; reszta to dopisanie bliźniaczych reguł `x86_64-*-nanos*` (mirror
istniejących i686) + 64-bitowy wariant `nanos.h`. Picolibc oficjalnie wspiera x86_64.
Żadnej „badań naukowych" tu nie ma.

**Werdykt:** nie ma przeszkody *blokującej*. Jest **jedna decyzja projektowa** (#1:
model kodu / niskie bazy / relokacje) z konsekwencjami dla `mknx`, linker-skryptów i
układu VA user — ją trzeba świadomie podjąć **zanim** ruszy się userland. Dwie nowe
ścieżki integracyjne nieobecne w i386 (#2 `%fs`, #3 SSE) to znane wzorce, ale łatwe do
przeoczenia. Reszta (toolchain-mirror + większość apek) jest mechaniczna.

---

## 6. Oszacowanie wysiłku (zgrubne, względne)

| Obszar | Charakter | Wielkość | Ryzyko |
|---|---|---|---|
| Trampolina long-mode w `loader.s` | nowy asm | ~100 linii | średnie (klasyczny wzorzec) |
| GDT/IDT/TSS 64-bit | struktury + init | ~200 linii | niskie |
| Ścieżka przerwań (isr/irq/switch/nxjmp) | przepisanie asm + TrapFrame | ~400 linii | średnie (red zone, swapgs) |
| Paging 4-poziomowy (`AddressSpace`, `mmu`, `Paging`) | redesign struktur | ~500 linii | **wysokie** (ale host-testowalne) |
| Syscall przez `syscall`/`sysret` + numery x86_64 | MD + tablica | ~200 linii | średnie |
| Userland ABI (crt0, syscall wrappers, sigtramp) | przepisanie asm/C | ~200 linii | średnie |
| Format `.nxe` v4 + `mknx` + `NxeLoader` | format + narzędzie | ~300 linii | średnie |
| MI cleanup LP64 (§4) | mechaniczne typy | rozproszone | niskie (łapie kompilator+testy) |
| Build/Docker/linker/QEMU | konfiguracja | — | niskie |
| **Toolchain `x86_64-nanos` + rebuild ~12 aplikacji** | porty | **bardzo duże** | **wysokie** |

**Sumarycznie kernel+userland-core:** rzędu 1500–2000 linii nowych/zmienionych
(głównie asm + struktury), w dużej części rozwijalne TDD na hoście dzięki
`AddressSpace`/`NxeLoader`/`Syscalls` które są host-testowane. **Ekosystem aplikacji
przeważa** nad tym wielokrotnie pod względem kalendarzowym.

---

## 7. Rekomendowana kolejność (kamienie milowe)

1. **Toolchain:** `arch/x86_64/arch.mk` + drugi build `x86_64-elf` w Dockerze;
   `nasm -f elf64`, `OUTPUT_FORMAT(elf64-x86-64)`, `qemu-system-x86_64`. Cel: pusty
   `arch/x86_64` kompiluje się.
2. **Boot do long mode:** trampolina w `loader.s` (tymczasowy identity-map 2 MiB
   hugepages, PAE+LME+PG), 64-bit GDT, skok do 64-bitowego `kmain` drukującego znak na
   VGA. **Pierwszy widoczny dowód życia w QEMU.**
3. **Paging właściwy:** port `AddressSpace`/`mmu_x86`→`mmu_x86_64` (4 poziomy, NX),
   rozwijany **TDD na hoście** (te pliki mają testy). Identity/low-half na start.
4. **Przerwania:** GDT/IDT/TSS 64-bit, `isr/irq` z ręcznym zrzutem rejestrów, `iretq`,
   IST dla #DF/#PF. Klawiatura/PIT/IRQ działają. ✅ zrealizowane w
   plans/2026-06-15-x86_64-plan-4-interrupts.md
5. **Storage:** `ATA.S`→64-bit (drobne), boot z ext4 — odtworzenie obecnej
   funkcjonalności read/write na 64-bit.
6. **Syscalle + userland:** `syscall`/`sysret`, numery x86_64, crt0/wrappery, format
   `.nxe` v4, `mknx`/`NxeLoader`. Uruchomienie `init.nxe` 64-bit.
7. **MI cleanup LP64** (§4) — równolegle, sterowane `-Wconversion` i test suite.
8. **`x86_64-nanos` SDK + porty aplikacji** — osobny, najdłuższy strumień (pełna
   mechanika w §5.3). Sekwencja zależności: **(8a)** in-tree userland + `libc.ndl`
   64-bit (już z kamienia 6) → **(8b)** patch `nanos-sdk/toolchain` o tuple
   `x86_64-*-nanos` + rebuild toolchaina i picolibc → **(8c)** rebuild portów,
   wewnętrznie równolegle: bash → grep/vim → sieć (inetutils) → OpenSSL/Dropbear →
   NetSurf/Doom. Całkowicie odseparowane od kernela — startuje po tym, jak x86_64
   bootuje i odpala `init.nxe`.

Każdy kamień milowy daje działający, weryfikowalny w QEMU artefakt — zgodnie ze stylem
TDD i headless-QEMU verification opisanym w CLAUDE.md.

---

## 8. Główne ryzyka i pułapki

- **Red zone (128 B poniżej `rsp`)** w handlerach przerwań/syscalli — łatwo o trudny
  do debugowania korupt stosu.
- **`swapgs`** — poprawne użycie przy granicy user/kernel; pomyłka = GP/korupcja.
- **Adresy kanoniczne** — bity 48–63 muszą być znak-rozszerzeniem bitu 47; literałowe
  okna VA z `mmu_x86` przeniesione naiwnie mogą wpaść w region niekanoniczny.
- **`struct stat`/ABI picolibc** — rozjazd layoutu i386 vs x86_64 to klasyczne źródło
  cichych błędów (złe pola, złe rozmiary).
- **Multiboot1 + >4 GiB** — świadomy limit; udokumentować, ewentualnie Multiboot2.
- **Dwa równoległe toolchainy w Dockerze** — czas builda i rozmiar obrazu; trzymać
  x86_64 w osobnych warstwach cache.

---

## 9. Co już gra na naszą korzyść

- Czysty podział MI/MD i przełącznik `ARCH` — port to *dodanie* arch, nie refaktor.
- MI-kod **już kompilowany i testowany jako LP64** (testy host-owe natywne).
- Kluczowe moduły do portu (`AddressSpace`, `NxeLoader`, `Syscalls`) są
  **host-testowane** → port rozwijalny TDD bez QEMU.
- Brak `-m32` — bitowość z triplety, więc nowy toolchain „po prostu" daje 64-bit.
- Port I/O, PCI, VGA, ATA — semantyka identyczna w long mode (minimalne zmiany).
- Headless-QEMU verification i bramka pokrycia 90% już istnieją jako siatka
  bezpieczeństwa.
