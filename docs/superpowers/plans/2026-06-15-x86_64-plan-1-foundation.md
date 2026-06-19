# x86_64 Plan 1 — Fundament: toolchain + boot do long mode

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `ARCH=x86_64 make bringup64` produkuje 64-bitowy (elf64) obraz, który GRUB/QEMU
ładuje w 32-bitowym trybie chronionym, a który sam buduje tablice stron, wchodzi w long
mode i wykonuje **64-bitowy kod C** wypisujący komunikat na VGA — pierwszy dowód życia
x86_64, bez ruszania kodu MI.

**Architecture:** Migracja-zastąpienie (patrz `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`,
§0.1). Zachowujemy przełącznik `ARCH` i podział MI/MD; tworzymy równoległy `arch/x86_64/`
i drugi cross-toolchain `x86_64-elf` w tym samym obrazie Dockera, **nie ruszając
istniejącego i686** (oba zielone w okresie przejściowym). Plan 1 celowo **omija
MI_SOURCES** — linkuje tylko `loader.o` + `entry64.o` przez dedykowany cel `bringup64`,
żeby był samodzielnym, testowalnym kawałkiem niezależnym od pełnego portu MI.

> **✅ ZREALIZOWANE (2026-06-16)** na gałęzi `feat/x86_64-foundation` (commity b986cb4..d9f514d).
> QEMU + GRUB w QEMU bootują 64-bitowy `bin/kernel64.bin` i wypisują „NanOS x86_64 long mode OK"
> na zielonym tle (screendump /tmp/x86_64-boot.png). i686 bez regresji (`make build` + `make
> check-arch` zielone).
>
> **Dwa odchylenia od pierwotnego planu (oba wymuszone realiami, nie skróty):**
> 1. **Boot przez GRUB ISO, nie `qemu -kernel`.** QEMU's multiboot1 `-kernel` loader odrzuca
>    ELF64 („Cannot load x86-64 image, give a 32bit one"). GRUB's multiboot1 loader ELF64 PRZYJMUJE,
>    więc `bringup64` buduje minimalne rescue-ISO (`grub-mkrescue`, menuentry `multiboot
>    /boot/kernel64.bin`) i bootuje `qemu-system-x86_64 -cdrom bin/nanos64.iso`. Entry point wyszedł
>    0x100010 (nie 0x100000) bo `.boot`/nagłówek multiboot (12 B) poprzedza `.text` — poprawne.
> 2. **`loader64.o`, nie `loader.o`.** i686 i x86_64 oba generują `loader.o` w współdzielonym `bin/`;
>    obiekt elf64 zatruwał link i686 („file format not recognized"). `_bringup64` ma jawny recipe
>    budujący do `bin/loader64.o`, więc kolizji nie ma.
>
> Task 7 Step 2/3 (adnotacja w `…-migration-analysis.md` + jej commit) — spec jest plikiem
> untracked wykluczonym ze stage'owania, więc odhaczenie zapisane tylko tutaj.

**Tech Stack:** Debian trixie cross-toolchain (binutils 2.43 + gcc 14.2.0,
`x86_64-elf`), NASM (`-f elf64`), GNU ld, QEMU (`qemu-system-x86_64`), Docker
(`nanos-build`). Wzorzec trampoliny long-mode = kanoniczny OSDev „Setting Up Long Mode".

**Reference:** Decyzje #1 (model kodu/relokacje), #2 (`%fs`), #3 (SSE) z §5.4 specu
NIE dotyczą Planu 1 (brak userlandu, brak picolibc) — pojawią się w Planie 6. Tu kernel
buduje się w modelu small (kod < 2 GiB @ 0x100000), `-mno-red-zone -mno-sse -mno-mmx`.

---

## File Structure

| Plik | Odpowiedzialność | Akcja |
|---|---|---|
| `docker/Dockerfile` | dołożyć build toolchaina `x86_64-elf` (warstwa na końcu, nie psuje cache i686) | Modify |
| `arch/x86_64/arch.mk` | knoby MD dla x86_64: `CROSS`, `ASM_FMT`, `QEMU`, `QEMU_CPU`, `KARCHFLAGS`, VPATH/INCLUDES/LINKER, minimalne `ARCH_SOURCES` | Create |
| `arch/x86_64/linker.ld` | skrypt linkera elf64, multiboot najpierw, `.text` @ 1 MiB | Create |
| `arch/x86_64/boot/loader.S` | NASM elf64: nagłówek Multiboot1, wejście 32-bit, tablice stron, PAE+LME+PG, GDT64, far-jump do long mode | Create |
| `arch/x86_64/boot/entry64.cpp` | minimalne 64-bit C entry: dowód, że toolchain linkuje freestanding C++ i że jesteśmy w long mode | Create |
| `arch/x86/arch.mk` | dodać puste/i686 wartości nowych zmiennych (`ASM_FMT=elf`, `QEMU=qemu-system-i386`, `KARCHFLAGS=`) — utrzymać i686 zielony | Modify |
| `Makefile` | sparametryzować regułę NASM (`-f $(ASM_FMT)`), binarkę QEMU (`$(QEMU)`), `QEMU_CPU ?=`, dodać `$(KARCHFLAGS)` do CXXFLAGS, cele `bringup64` (host) + `_bringup64` (kontener) | Modify |

---

## Task 1: Drugi cross-toolchain `x86_64-elf` w Dockerze

**Files:**
- Modify: `docker/Dockerfile:98` (wstawić nową warstwę tuż przed `RUN touch /etc/nanos-build`)

- [x] **Step 1: Dodać build toolchaina x86_64-elf jako trailing layer**

Wstaw PRZED linią `# Marker the Makefile checks…` (obecnie `docker/Dockerfile:99`):

```dockerfile
# Second cross-toolchain: x86_64-elf (the long-mode port target). Built from the SAME
# pinned binutils/gcc sources as i686-elf, into the same PREFIX (the triple-prefixed
# binaries don't collide: x86_64-elf-gcc vs i686-elf-gcc). Appended as a trailing layer
# so it never invalidates the cached i686-elf toolchain above. ~20-40 min first build
# (amd64 emulated on Apple Silicon), then cached.
ARG TARGET64=x86_64-elf
RUN set -eux; \
    cd /tmp; \
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"; \
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"; \
    tar xf "binutils-${BINUTILS_VERSION}.tar.xz"; \
    tar xf "gcc-${GCC_VERSION}.tar.xz"; \
    mkdir build-binutils64; cd build-binutils64; \
    ../binutils-${BINUTILS_VERSION}/configure \
        --target="${TARGET64}" --prefix="${PREFIX}" \
        --with-sysroot --disable-nls --disable-werror; \
    make -j"$(nproc)"; make install; \
    mkdir /tmp/build-gcc64; cd /tmp/build-gcc64; \
    ../gcc-${GCC_VERSION}/configure \
        --target="${TARGET64}" --prefix="${PREFIX}" \
        --disable-nls --enable-languages=c,c++ --without-headers \
        --disable-libssp --disable-libsanitizer; \
    make -j"$(nproc)" all-gcc all-target-libgcc; \
    make install-gcc install-target-libgcc; \
    cd /; rm -rf /tmp/*
```

- [x] **Step 2: Przebudować obraz buildowy**

Run: `docker build -t nanos-build -f docker/Dockerfile docker/`
Expected: build przechodzi (długi — drugi gcc od zera); kończy się bez błędu.

- [x] **Step 3: Zweryfikować, że nowy toolchain istnieje i ma poprawny target**

Run: `docker run --rm nanos-build x86_64-elf-gcc -dumpmachine`
Expected: `x86_64-elf`

Run: `docker run --rm nanos-build sh -c 'i686-elf-gcc -dumpmachine && x86_64-elf-gcc -dumpmachine'`
Expected (oba toolchainy obecne):
```
i686-elf
x86_64-elf
```

- [x] **Step 4: Commit**

```bash
git add docker/Dockerfile
git commit -m "build: add x86_64-elf cross toolchain alongside i686-elf"
```

---

## Task 2: Szkielet `arch/x86_64/` — arch.mk + linker.ld

**Files:**
- Create: `arch/x86_64/arch.mk`
- Create: `arch/x86_64/linker.ld`

- [x] **Step 1: Utworzyć `arch/x86_64/arch.mk`**

```makefile
# arch/x86_64/arch.mk — machine-dependent build knobs for x86_64 (long mode).
#
# Mirrors arch/x86/arch.mk. Selected by the top-level Makefile as arch/$(ARCH)/arch.mk
# when invoked with ARCH=x86_64. During the migration both arches coexist; this file
# is the single place x86_64 toolchain/format choices live.

CROSS ?= x86_64-elf-

# NASM object format and the QEMU binary for this arch (consumed by the Makefile).
ASM_FMT ?= elf64
QEMU     ?= qemu-system-x86_64
QEMU_CPU ?= -cpu qemu64

# Kernel-only codegen flags for x86_64: no SSE/MMX (kernel must not touch XMM across
# interrupts), no red zone (interrupt handlers would clobber it). Small code model is
# fine while the kernel lives low (@ 0x100000, < 2 GiB).
KARCHFLAGS ?= -mno-red-zone -mno-sse -mno-mmx -mno-80387

ARCH_VPATH=arch/x86_64/boot:arch/x86_64/cpu:arch/x86_64/mm:arch/x86_64/drivers:arch/x86_64/io
ARCH_INCLUDES=-Iarch/x86_64/boot -Iarch/x86_64/cpu -Iarch/x86_64/mm -Iarch/x86_64/drivers -Iarch/x86_64/io
ARCH_LINKER=arch/x86_64/linker.ld

# Plan 1 bring-up: only the boot trampoline + a 64-bit C entry stub. The full MD set
# (Gdt/Idt/interrupts/paging/drivers) is filled in by later plans.
ARCH_SOURCES=loader.o entry64.o
```

- [x] **Step 2: Utworzyć `arch/x86_64/linker.ld`**

```ld
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(loader)
SECTIONS
{
  . = 0x100000;                 /* GRUB/QEMU load the kernel at 1 MiB, 32-bit PM entry */
  .boot   : { *(.multiboot) }   /* Multiboot header first, within GRUB's 8 KiB scan window */
  .text   : { *(.text) *(.text.*) }
  .rodata : { *(.rodata) *(.rodata.*) }
  .data   : { *(.data) *(.data.*) }
  .bss    : { *(.bss) *(.bss.*) *(COMMON) }
  end = .; _end = .; __end = .;
}
```

- [x] **Step 3: Sanity — make parsuje nowy arch.mk**

Run: `make ARCH=x86_64 -n bringup64 2>&1 | head -5`
Expected: brak błędu „No such file or directory" dla `arch/x86_64/arch.mk`; make wypisuje plan komend (cel `bringup64` powstanie w Tasku 3, więc na tym etapie dopuszczalny błąd „No rule to make target `bringup64'" — KLUCZOWE jest, że arch.mk się włączył bez błędu include).

- [x] **Step 4: Commit**

```bash
git add arch/x86_64/arch.mk arch/x86_64/linker.ld
git commit -m "x86_64: add arch.mk + elf64 linker script skeleton"
```

---

## Task 3: Parametryzacja Makefile (ASM_FMT, QEMU, KARCHFLAGS, cel bring-up)

**Files:**
- Modify: `arch/x86/arch.mk:10` (dodać i686 wartości nowych zmiennych)
- Modify: `Makefile:281` (`QEMU_CPU ?=`), `Makefile:288/291/312` (`$(QEMU)`), `Makefile:377` (`$(KARCHFLAGS)`), `Makefile:416` (`-f $(ASM_FMT)`), + nowe cele

- [x] **Step 1: Dodać do `arch/x86/arch.mk` domyślne wartości nowych zmiennych**

Po linii `CROSS ?= i686-elf-` (`arch/x86/arch.mk:10`) dodaj:

```makefile

# Build knobs shared with the top-level Makefile (mirrored in arch/x86_64/arch.mk).
ASM_FMT    ?= elf
QEMU       ?= qemu-system-i386
QEMU_CPU   ?= -cpu Nehalem
KARCHFLAGS ?=
```

- [x] **Step 2: Zmienić `Makefile:281` na `?=` (żeby arch.mk mógł ustawić CPU)**

Zamień (`Makefile:281`):
```makefile
QEMU_CPU=-cpu Nehalem
```
na:
```makefile
# Default set by arch/$(ARCH)/arch.mk (i686 -> Nehalem, x86_64 -> qemu64); ?= lets the
# arch override win since arch.mk is included earlier.
QEMU_CPU ?= -cpu Nehalem
```

- [x] **Step 3: Użyć `$(QEMU)` w celach uruchomieniowych**

W `Makefile` zamień wszystkie trzy wystąpienia `qemu-system-i386` (linie 288, 291, 312) na `$(QEMU)`. Przykład dla l. 288:
```makefile
run: image
	$(QEMU) $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE_GRUB2),format=raw $(NIC_NET)
```
(analogicznie `run-iso` l. 291: `$(QEMU) -cdrom nanos.iso`, oraz `run-net` l. 312.)

- [x] **Step 4: Dodać host-side cel `bringup64` (sekcja HOST, przed `else` z l. 341)**

Wstaw przed `else` (`Makefile:341`), w sekcji host:
```makefile
# x86_64 bring-up (Plan 1): build the minimal long-mode boot image in the container,
# then boot it natively. No GRUB image needed — QEMU's -kernel loads the multiboot ELF.
.PHONY: bringup64
bringup64:
	$(DOCKER_RUN) make ARCH=x86_64 _bringup64
	@echo "Booting bin/kernel64.bin — expect 'NanOS x86_64 long mode OK' on the VGA console."
	$(QEMU) $(QEMU_CPU) $(QEMU_MEM) -kernel $(BINFOLDER)kernel64.bin
```

- [x] **Step 5: Dodać `$(KARCHFLAGS)` do CXXFLAGS (kontener, `Makefile:377`)**

Zamień (`Makefile:377`):
```makefile
CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore $(KOPTFLAGS)
```
na (dodane `$(KARCHFLAGS)`):
```makefile
CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore $(KARCHFLAGS) $(KOPTFLAGS)
```

- [x] **Step 6: Sparametryzować regułę NASM (`Makefile:416`)**

Zamień (`Makefile:415-416`):
```makefile
$(BINFOLDER)%.o: %.S
	nasm -f elf $< -o $@
```
na:
```makefile
$(BINFOLDER)%.o: %.S
	nasm -f $(ASM_FMT) $< -o $@
```

- [x] **Step 7: Dodać kontener-side cel `_bringup64` (po regule `_compile`, ~`Makefile:401`)**

Wstaw po bloku `_compile`/`kernel.bin` (po `Makefile:404`):
```makefile
# Minimal long-mode bring-up link (Plan 1): ONLY the boot trampoline + 64-bit C entry,
# bypassing MI_SOURCES. loader.S / entry64.cpp don't include <string.h>, so they compile
# directly in /src without the case-insensitive-FS copy dance.
_bringup64: $(BINFOLDER)loader.o $(BINFOLDER)entry64.o
	$(LD) -T$(ARCH_LINKER) -nostdlib -nostartfiles -o $(BINFOLDER)kernel64.bin $(BINFOLDER)loader.o $(BINFOLDER)entry64.o
```

- [x] **Step 8: Potwierdzić, że i686 nadal się buduje (regresja)**

Run: `make build`
Expected: build kernela i686 przechodzi jak dotąd (nowe zmienne są no-opem dla x86; `$(KARCHFLAGS)` puste, `-f $(ASM_FMT)` = `-f elf`).

- [x] **Step 9: Commit**

```bash
git add Makefile arch/x86/arch.mk
git commit -m "build: parameterize ASM_FMT/QEMU/KARCHFLAGS + add x86_64 bringup64 target"
```

---

## Task 4: Trampolina long-mode `arch/x86_64/boot/loader.S`

**Files:**
- Create: `arch/x86_64/boot/loader.S`

- [x] **Step 1: Napisać trampolinę (NASM, sekcje 32-bit → 64-bit)**

```nasm
; arch/x86_64/boot/loader.S — Multiboot1 entry + 32->64 long-mode trampoline (NASM, elf64).
; GRUB/QEMU enter here in 32-bit protected mode (multiboot). We build a temporary
; identity page map (1 GiB via 2 MiB huge pages), enable PAE + EFER.LME + paging, load a
; 64-bit GDT, far-jump into long mode, then call the 64-bit C entry kentry64(mb_info).

MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
MBFLAGS  equ MBALIGN | MEMINFO
MBMAGIC  equ 0x1BADB002
MBCHECK  equ -(MBMAGIC + MBFLAGS)

section .multiboot
align 4
    dd MBMAGIC
    dd MBFLAGS
    dd MBCHECK

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
align 4096
p4_table:                       ; PML4
    resb 4096
p3_table:                       ; PDPT
    resb 4096
p2_table:                       ; PD (512 * 2 MiB = 1 GiB identity)
    resb 4096

section .text
bits 32
global loader
extern kentry64
loader:
    mov esp, stack_top
    mov edi, ebx                ; preserve multiboot info ptr -> becomes rdi (arg0) in long mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp  gdt64.code:long_mode_start   ; far jump activates 64-bit mode

setup_page_tables:
    mov eax, p3_table
    or  eax, 0b11               ; present | writable
    mov [p4_table], eax
    mov eax, p2_table
    or  eax, 0b11
    mov [p3_table], eax
    xor ecx, ecx                ; ecx = page index 0..511
.map_p2:
    mov eax, 0x200000           ; 2 MiB
    mul ecx                     ; edx:eax = 2 MiB * ecx (< 1 GiB so edx = 0)
    or  eax, 0b10000011         ; present | writable | huge (PS bit 7)
    mov [p2_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_p2
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax                ; CR3 = PML4 phys
    mov eax, cr4
    or  eax, 1 << 5             ; CR4.PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; IA32_EFER MSR
    rdmsr
    or  eax, 1 << 8             ; EFER.LME (long mode enable)
    wrmsr
    mov eax, cr0
    or  eax, 1 << 31            ; CR0.PG (paging) -> activates long mode
    mov cr0, eax
    ret

bits 64
long_mode_start:
    mov ax, 0                   ; null the data segment regs (ignored in long mode)
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top
    call kentry64               ; rdi already holds the multiboot info ptr (from edi)
.hang:
    hlt
    jmp .hang

section .rodata
gdt64:
    dq 0                                            ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)        ; exec | type | present | long-mode (L)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
```

- [x] **Step 2: Zbudować sam obiekt (weryfikacja składni NASM elf64)**

Run: `docker run --rm -v $(pwd):/src -w /src nanos-build nasm -f elf64 arch/x86_64/boot/loader.S -o /tmp/loader.o && echo OK`
Expected: `OK` (brak błędów asemblacji).

- [x] **Step 3: Commit**

```bash
git add arch/x86_64/boot/loader.S
git commit -m "x86_64: add 32->64 long-mode boot trampoline"
```

---

## Task 5: 64-bitowe wejście C `arch/x86_64/boot/entry64.cpp`

**Files:**
- Create: `arch/x86_64/boot/entry64.cpp`

- [x] **Step 1: Napisać minimalne wejście C++**

```cpp
// arch/x86_64/boot/entry64.cpp — minimal 64-bit C++ entry point reached from the
// long-mode trampoline (loader.S). Its sole job in Plan 1: prove the x86_64-elf
// toolchain compiles/links freestanding C++ AND that we are genuinely executing 64-bit
// code in long mode, by writing a message straight to the VGA text buffer at 0xB8000.
// The full MI kmain wiring comes in later plans.

extern "C" void kentry64(unsigned long mb_info) {
    (void) mb_info;  // multiboot info ptr — unused in Plan 1, wired in Plan 2
    volatile unsigned short* vga = (volatile unsigned short*) 0xB8000;
    const char* msg = "NanOS x86_64 long mode OK";
    unsigned i = 0;
    for (; msg[i] != '\0'; ++i)
        vga[i] = (unsigned short) (0x2F00 | (unsigned char) msg[i]);  // 0x2F = white on green
    // clear the rest of the first row so a stale framebuffer doesn't confuse the eye
    for (; i < 80; ++i)
        vga[i] = 0x2F20;  // space, same attribute
    for (;;)
        __asm__ __volatile__("hlt");
}
```

- [x] **Step 2: Zbudować sam obiekt (weryfikacja flag x86_64 kernela)**

Run:
```bash
docker run --rm -v $(pwd):/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  arch/x86_64/boot/entry64.cpp -o /tmp/entry64.o && echo OK
```
Expected: `OK` (kompiluje się freestanding, bez FP/SSE).

- [x] **Step 3: Commit**

```bash
git add arch/x86_64/boot/entry64.cpp
git commit -m "x86_64: add minimal 64-bit C entry (VGA proof-of-life)"
```

---

## Task 6: Złożyć obraz i zweryfikować boot w QEMU

**Files:** (brak nowych — używa celów z Tasków 2-5)

- [x] **Step 1: Zbudować obraz bring-up**

Run: `make ARCH=x86_64 bringup64` — część kontenerowa (`_bringup64`) zbuduje
`bin/loader.o`, `bin/entry64.o` i zlinkuje `bin/kernel64.bin`, po czym host odpali QEMU.

Alternatywnie sam build bez bootowania:
Run: `docker run --rm -v $(pwd):/src -w /src nanos-build make ARCH=x86_64 _bringup64 && ls -l bin/kernel64.bin`
Expected: powstaje `bin/kernel64.bin` (ELF64); `file bin/kernel64.bin` → `ELF 64-bit LSB executable, x86-64`.

- [x] **Step 2: Potwierdzić, że to multiboot ELF (QEMU -kernel go przyjmie)**

Run: `docker run --rm -v $(pwd):/src -w /src nanos-build sh -c 'x86_64-elf-readelf -h bin/kernel64.bin | grep -E "Class|Machine|Entry"'`
Expected:
```
Class:                             ELF64
Machine:                           Advanced Micro Devices X86-64
Entry point address:               0x100000
```

- [x] **Step 3: Boot interaktywny — zobaczyć komunikat**

Run: `qemu-system-x86_64 -cpu qemu64 -m 512 -kernel bin/kernel64.bin`
Expected: w oknie QEMU, w lewym górnym rogu, na zielonym tle: `NanOS x86_64 long mode OK`.
(Maszyna następnie wisi na `hlt` — to oczekiwane dla Planu 1.)

- [x] **Step 4: Weryfikacja headless (powtarzalna, wzorzec z CLAUDE.md)**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -kernel bin/kernel64.bin \
  -display none -monitor unix:/tmp/qmon64,server,nowait &
QEMU_PID=$!
sleep 2
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon64")
s.recv(4096)
s.sendall(b"screendump /tmp/x86_64-boot.ppm\n")
import time; time.sleep(1)
PY
kill $QEMU_PID 2>/dev/null
sips -s format png /tmp/x86_64-boot.ppm --out /tmp/x86_64-boot.png >/dev/null
echo "Open /tmp/x86_64-boot.png — expect 'NanOS x86_64 long mode OK' in row 0."
```
Expected: PNG pokazuje komunikat w wierszu 0. (Jeśli ekran czarny/triple-fault: dodaj
`-no-reboot -d int -D /tmp/qlog` i sprawdź `grep -E "v=0d|v=08" /tmp/qlog` — GP/triple
fault wskazuje błąd w tablicach stron lub GDT.)

- [x] **Step 5: Commit (jeśli były drobne poprawki w Tasku 6)**

```bash
git add -A
git commit -m "x86_64: verify long-mode boot reaches 64-bit C entry in QEMU" --allow-empty
```

---

## Task 7: Domknięcie planu

- [x] **Step 1: Potwierdzić brak regresji i686**

Run: `make build && make check-arch`
Expected: kernel i686 buduje się; `OK: MI layer is arch-clean.`

- [x] **Step 2: Zaktualizować spec — odhaczyć kamień 1-2 (boot do long mode)**

W `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`, §7, dopisać przy
kamieniach 1-2 adnotację „✅ zrealizowane w plans/2026-06-15-x86_64-plan-1-foundation.md".

- [x] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md
git commit -m "docs: mark x86_64 milestones 1-2 (toolchain + long-mode boot) done"
```

---

## Self-Review (wykonane)

- **Spec coverage:** Plan realizuje kamienie 1 (toolchain/build) i 2 (boot do long mode)
  z §7 specu. Decyzje #1/#2/#3 z §5.4 świadomie odłożone do Planu 6 (brak userlandu tutaj).
- **Placeholders:** brak — każdy krok ma realny kod/komendę i oczekiwany wynik.
- **Type/naming consistency:** `kentry64(unsigned long)` zadeklarowane w loader.S
  (`extern kentry64`) i zdefiniowane w entry64.cpp z tą samą sygnaturą; `ASM_FMT`,
  `QEMU`, `QEMU_CPU`, `KARCHFLAGS` zdefiniowane w obu arch.mk i użyte w Makefile spójnie;
  `ARCH_LINKER`/`ARCH_SOURCES` zgodne z istniejącym wzorcem x86.

---

## Następne plany (2–8) — indeks roadmapy

Każdy to osobny plik `docs/superpowers/plans/2026-06-15-x86_64-plan-N-*.md`, rozpisywany
w pełni dopiero gdy poprzednik ląduje (asm/paging wymaga iteracji na działającym
fundamencie). Cel + zakres + kryterium wyjścia:

- **Plan 2 — Pełny boot + konsola + MI kmain.** Doprowadzić 64-bit kmain do realnego
  `Kernel::start`: console_x86_64 (sink VGA), parsing multiboot (mmap/framebuffer w
  64-bit), wejście w MI `kmain()`. *Exit:* kernel wypisuje przez `Console::write*` i
  dobija do pętli idle. Wymaga okrojonego MI (bez storage/sched) — staged.
- **Plan 3 — Paging 4-poziomowy (PML4).** Port `Paging.h`/`PagingControl.h`/
  `AddressSpace`/`mmu_x86_64` na 4 poziomy + bit NX; **rozwijany TDD na hoście** (te
  moduły mają testy doctest). *Exit:* host-testy AddressSpace 64-bit zielone + kernel
  mapuje własną przestrzeń i identity-mapuje RAM.
- **Plan 4 — GDT/IDT/TSS + przerwania.** IDT 16-bajtowe gaty, TSS 64-bit z IST, `isr/irq`
  z ręcznym zrzutem rax–r15, `iretq`, `swapgs`, nowy `Registers` (TrapFrame). *Exit:*
  klawiatura (IRQ1) + PIT (IRQ0) działają; #PF/#GP raportowane.
- **Plan 5 — Storage.** `ATA.S` 64-bit (drobne), `block_x86_64`; boot z ext4. *Exit:*
  odtworzona obecna funkcjonalność read/write na 64-bit (e2fsck-clean).
- **Plan 6 — Syscalle + userland + `.nxe` v4.** `syscall/sysret` (MSR LSTAR/STAR/FMASK),
  numery x86_64, `%fs.base` dla user (decyzja #2), crt0/wrappery (#3 SSE), format `.nxe`
  v4 + `mknx` (decyzja #1: model small, niskie bazy, `R_X86_64_64`/`R_X86_64_32S`),
  `NxeLoader` 64-bit. *Exit:* `init.nxe` 64-bit startuje i dobija do nsh.
- **Plan 7 — MI cleanup LP64.** Sweep §4 specu pod `-Wconversion`: FrameAllocator,
  `kernelSyscall` args→`uintptr_t`, Console hex 64-bit, rzutowania adresów MMIO/BAR/FB,
  `off_t` w Fd/lseek. *Exit:* `-Wconversion` czyste w MI; test suite host zielony.
- **Plan 8 — SDK `x86_64-nanos` + porty aplikacji.** (a) patch `nanos-sdk/toolchain` o
  tuple `x86_64-*-nanos` + rebuild toolchaina/picolibc; (b) rebuild ~12 portów (bash →
  grep/vim → sieć → OpenSSL/Dropbear → NetSurf/Doom). Pełna mechanika: spec §5.3-§5.4.
  *Exit:* obraz dysku z 64-bitowymi apkami bootuje i odpala bash/nsh.
