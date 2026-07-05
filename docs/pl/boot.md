# Sekwencja startowa NanOS

Od włączenia zasilania do powłoki: GRUB2 ładuje kernel Multiboot, `loader.s` ustawia stos i
woła `kmain`, a `Kernel::start` uruchamia maszynę podsystem po podsystemie, zanim odda
CPU schedulerowi, który uruchamia `init` w ring 3. Ten dokument prześledzi tę drogę. To, co robi
każdy podsystem po uruchomieniu, opisują dokumenty poszczególnych podsystemów (memory.md, scheduler.md,
filesystem.md, terminal.md).

```
GRUB2 ──multiboot──► loader.s (0x100000) ──► kmain() ──► Kernel::start()
  reads /nanos/core/kernel.bin   save magic+mbd      MI entry      ordered bring-up ──► Scheduler::start()
  from the ext partition         set ESP, call kmain               (CPU→paging→FS→dev→…)      └─► init.nxe (ring 3)
```

---

## 1. GRUB2 + Multiboot

GRUB2 jest zainstalowany w obrazie dysku (`scripts/create-image.sh`): kod startowy MBR + rdzeń
`grub-mkimage` (z modułami `multiboot`, `part_msdos`, `ext2`, `biosdisk`) ładuje
`/boot/grub/grub.cfg`, który wykonuje `multiboot /nanos/core/kernel.bin` — GRUB czyta kernel z
partycji **ext** i skacze do niego zgodnie z protokołem Multiboot.

Kernel jest binarką Multiboot, ponieważ `arch/x86/boot/loader.s` umieszcza **nagłówek Multiboot** w
sekcji `.multiboot`, którą `arch/x86/linker.ld` umieszcza **jako pierwszą**, pod adresem ładowania `0x100000` (1 MiB),
tak by skan nagłówków GRUB ją znalazł. Nagłówek ustawia `magic 0x1BADB002` i flagi `ALIGN | MEMINFO |
VIDEO` — flaga VIDEO **żąda liniowego framebuffera** (1024×768×32), dzięki czemu NanOS dostaje
konsolę graficzną. Przy przekazaniu sterowania GRUB podaje `eax = 0x2BADB002` (boot magic) oraz `ebx` = wskaźnik
do struktury `MultibootInfo` (mapa pamięci + informacje o framebufferze).

---

## 2. Wejście w asemblerze (`arch/x86/boot/loader.s`, GNU as / AT&T)

`ENTRY(loader)`:

1. rezerwuje stos startowy (`stack_bottom … stack_top`) i ustawia `esp`;
2. zapisuje dane przekazane przez Multiboot: `magic` ← `eax`, `mbd` ← `ebx` (globalne zmienne czytane
   później przez warstwę boot-info);
3. `call kmain` — a jeśli kiedykolwiek wróci, `cli; hlt` na zawsze.

Dostarcza też stuby ładowania deskryptorów wołane po stronie C++: `gdt_flush` (ładuje GDTR, przeładowuje
rejestry segmentów danych na `0x10`, far-jump w celu przeładowania `cs = 0x08`) oraz `idt_load` (ładuje IDTR, `sti`).

---

## 3. Boot info: mapa pamięci + framebuffer

`arch/x86/boot/` parsuje to, co przekazał GRUB, i wystawia przez kontrakt `<arch/bootinfo.h>`
(`bootinfo_x86.cpp`):

- **`MultibootInfo.h`** — spakowana struktura Multiboot (flags, `mmap_*`, `framebuffer_*`).
- **`MultibootMmap.cpp`** — przejście po buforze mapy pamięci (krok wpisu = `size + 4`), znalezienie najwyższego
  użytecznego (`type == 1`) adresu.
- **`bootMemTop()`** — szczyt użytecznego RAM (fallback 128 MiB, domyślny w QEMU).
- **`bootMemForEachUsable(cb)`** — iteracja po użytecznych zakresach (zasila frame allocator, memory.md).
- **`bootFramebuffer()`** — liniowy framebuffer RGB (addr/pitch/w/h/bpp), albo brak → tekstowe VGA.

---

## 4. `kmain` → `Kernel::start`

`init/kmain.cpp` to maleńkie wejście MI: utworzenie `Kernel`, wywołanie `start()`. `Kernel::start`
(`kernel/Kernel.cpp`) to uporządkowane uruchamianie. Każdy krok po wstaniu framebuffera wypisuje
linijkę w stylu Linuksa `[ OK ]` (`okBegin` pisze `[    ] msg`, krok się wykonuje, `okEnd` przepisuje znacznik
na zielono):

| # | Krok | Co |
|---|---|---|
| 1 | `arch::cpuInit()` | GDT + TSS, IDT + remap PIC, obsługa faultów (§5) |
| 2 | `setBootEpoch(arch::rtcEpoch())` | zasianie zegara ściennego z RTC (znaczniki czasu plików); zasianie CSPRNG |
| 3 | `initPaging()` | frame allocator z mmap, włączenie pagingu, mapowanie MMIO framebuffera, **przełączenie konsoli na fbcon** (memory.md, terminal.md) |
| — | boot splash | `NanOS -- booting` + `[ OK ]` dla CPU, Paging, Framebuffer |
| 4 | storage stack | rejestracja `bootDisk()`, `new Vfs`, rejestracja typów ext2/ext4, montowanie syntetycznego `/`, montowanie ext pod `/disks/main` (LBA partycji z MBR) |
| 5 | `extRwSelftest(vfs)` | zapis+odczyt `/disks/main/nanos/rwtest`, raport trwałości (filesystem.md §5) |
| 6 | `/tmp`, `/etc` | montowanie tmpfs RamFs; wypełnienie `/etc` z szablonu na dysku |
| 7 | węzły `/dev` | `fb0` (jeśli framebuffer), `input0` (evdev klawiatury), `ptmx`/`pts0`/`tty` (PTY) — terminal.md |
| 8 | PCI | instalacja backendu config-space + enumeracja (znajduje e1000) |
| 9 | `loadAllKexts("/disks/main/nanos/kext")` | ładowanie sterowników PS/2 + e1000 (kext.md) |
| 10 | `installSyscalls(vfs)` | podpięcie `int 0x80` + self-test (syscalls.md) |
| 11 | scheduler | `Scheduler::init()` (idle), utworzenie zadania `init` (pid 1), rejestracja wątków jądra idle + net, uruchomienie sieci, `archTimerInit(1000)` |
| 12 | `Scheduler::start()` | przełączenie do pierwszego zadania — nigdy nie wraca (scheduler.md) |

---

## 5. GDT / IDT / PIC — oraz poprawka na triple fault

`arch::cpuInit()` (`cpu_x86.cpp`) instaluje **własną GDT NanOS** (`Gdt.cpp`), ustawia stos jądra w TSS,
ładuje TSS, a następnie inicjalizuje IDT (`Idt.cpp`).

**Płaska GDT** — sześć wpisów, każdy obejmujący 0–4 GiB:

| Selector | Wpis | Zastosowanie |
|---|---|---|
| `0x00` | null | — |
| `0x08` | kod ring-0 | kernel; **selektor zakodowany na sztywno w każdej bramce IDT** |
| `0x10` | dane ring-0 | kernel |
| `0x1B` | kod ring-3 (DPL 3) | userspace |
| `0x23` | dane ring-3 (DPL 3) | userspace |
| `0x28` | TSS | przełączenie stosu ring3→ring0 (`esp0`) |

**IDT** — 256 bramek: 0–31 wyjątki CPU, 32–47 przemapowane sprzętowe IRQ, oraz **bramka 128
(`int 0x80`) z DPL 3**, by kod ring-3 mógł ją wywołać. `Idt::initialize` także **przemapowuje 8259 PIC**
master→`0x20`, slave→`0x28` (poza wektory wyjątków CPU), a następnie `idt_load` + `sti`.

> **Dlaczego własna GDT — pierwotny bug „GRUB2 nie startuje".** Bramki IDT zakodowane mają na sztywno selektor kodu
> `0x08`. GRUB2 przekazuje sterowanie ze *swoją* GDT, gdzie `0x08` nie jest segmentem kodu jądra, więc
> pierwsze sprzętowe IRQ było wektorowane przez zły selektor → #GP → #DF → **triple fault → reboot**.
> Poprawką jest dokładnie powyższa kolejność: zainstalować płaską GDT (tak by `0x08` był poprawnym kodem jądra) **zanim**
> IDT włączy przerwania. To bug #1 w CLAUDE.md i powód, dla którego `cpuInit` uruchamia się pierwszy.

---

## 6. Przekazanie sterowania do userspace (`init`, ring 3)

Pierwsze zadanie schedulera uruchamia `initTaskBody` (`Kernel.cpp`): pozwala splashowi `[ OK ]` pozostać ~2 s
(oddając CPU co tick), `clearScreen`, a następnie `execProgram(vfs, "/disks/main/nanos/core/init.nxe")` —
co ładuje `.nxe`, buduje jego przestrzeń adresową i wykonuje `iret` do **ring 3** (nxe-ndl.md §6;
to obecny model trap-frame — `init` jest normalnym procesem ring-3, a *nie* ring 0). `init.nxe`
uruchamia sieć przez DHCP, startuje usługi nasłuchujące i wykonuje `execve` na powłoce logowania
(writing-apps.md §3.7). Jeśli ładowanie się nie powiedzie, `initTaskBody` wypisuje błąd i wraca.

---

## 7. Artefakty startowe

`scripts/create-image.sh` (uruchamiany wewnątrz kontenera `nanos-build` przez `make image`) buduje
dysk raw o rozmiarze 32 MiB: MBR z jedną bootowalną partycją pod LBA 2048, na niej system plików **ext4**,
zapisany `grub.cfg`, oraz zainstalowane `boot.img` (MBR) + `core.img` (sektor 1) z GRUB2. Kernel
mieszka pod `/nanos/core/kernel.bin`, a stage'e bootloadera pod `/boot/grub/` (filesystem.md
§2). `make iso`/`run-iso` opakowują ten sam GRUB w bootowalne ISO przez `grub-mkrescue`.

**Kluczowe pliki:** `grub.cfg`, `arch/x86/boot/{loader.s, linker.ld, MultibootInfo.h, MultibootMmap.cpp,
bootinfo_x86.cpp}`, `init/kmain.cpp`, `kernel/Kernel.cpp`, `arch/x86/cpu/{cpu_x86.cpp, Gdt.cpp,
Idt.cpp}`, `scripts/create-image.sh`.
