# Pliki wykonywalne i biblioteki dzielone NanOS (`.nxe` / `.ndl`)

NanOS **nie** uruchamia ELF-a w runtime. Program kompiluje się do ELF-a, po czym hostowe narzędzie `mknx`
konwertuje go do **NxFormat** — `.nxe` dla programu, `.ndl` dla biblioteki dzielonej. Model linkowania jest
**w stylu Windows-PE / MinGW** (import-by-name, export-by-name, relokacje bazowe), a nie ELF/glibc.

Ten dokument to **referencja formatu + architektury runtime'u**: format na dysku, stos loaderów,
linkowanie dynamiczne, mapa pamięci userland oraz cykl życia procesu w ring 3. Po **przepis**
(jak napisać aplikację wbudowaną albo sportować realny program linuksowy z nanos-sdk) sięgnij do
writing-apps.md. Wariant modułu jądra `.nkext` jest w kext.md; gdzie pliki żyją
na dysku, opisuje filesystem.md.

---

## 1. Jeden format, trzy warianty

Wszystkie trzy współdzielą `kernel/NxFormat.h` (magic `"NXE"` = `0x0045584E`, `NX_VERSION 3`) oraz **ten sam
silnik loadera** (`NxeLoader`). Różnią się tylko sposobem użycia i tym, gdzie rozwiązują się importy:

| Rozszerzenie | Co to | Marker | Importy rozwiązywane wobec | Działa w |
|---|---|---|---|---|
| `.nxe` | program (executable) | — | załadowanych bibliotek `.ndl` | **ring 3** |
| `.ndl` | biblioteka dzielona | `NX_FLAG_DLL` | innych bibliotek `.ndl` | ring 3 (w procesie) |
| `.nkext` | moduł jądra | — | tabeli eksportów jądra | **ring 0** (patrz kext.md) |

> Kontrast z Linuksem: ELF + `ld.so` + GOT/PLT, symbole wiązane płasko przez `ELF hash`. NanOS to
> model Windows-PE — binarka zależy od **stabilnego, nazwanego API** (`libc.ndl`), a nie od konkretnej
> wersji biblioteki wkompilowanej na sztywno. Jedna binarka działa na każdym buildzie, który eksportuje
> to API: brak problemu „binarka na każdą dystrybucję".

---

## 2. Format

Na dysku (i w pamięci po załadowaniu pod `loadBase`):

```
[ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] | NxNeeded[] | strings | (bss — not stored) ]
```

Wszystkie adresy w nagłówku i tabelach są **absolutne** (względem `loadBase`). `NxHeader`
(`kernel/NxFormat.h`):

| Pole | Znaczenie |
|---|---|
| `magic` / `version` / `flags` | `NX_MAGIC`, `NX_VERSION 3`, `NX_FLAG_DLL` (bit 0) dla biblioteki. |
| `entry` | wejście programu (`.nxe` → `_start`). |
| `loadBase` | preferowana baza linkowania (`0x800000` dla programów; biblioteki są relokowane). |
| `imageSize` | bajtów zapisanych w pliku (nagłówek + code + rodata + data + tabele). |
| `bssStart` / `bssEnd` | zerowane przez loader, nie zapisywane. |
| `importTable` / `importCount` | `NxImport[]` — symbole do związania. |
| `exportTable` / `exportCount` | `NxExport[]` — symbole, które ten moduł dostarcza (biblioteki). |
| `relocTable` / `relocCount` | `NxReloc[]` — poprawki R_386_32. |
| `neededTable` / `neededCount` | `NxNeeded[]` — biblioteki `.ndl` do załadowania w pierwszej kolejności. |

Cztery typy wpisów tabel:

- **`NxImport { nameOff, slotAddr, libOff }`** — rozwiąż symbol `nameOff` w bibliotece nazwanej przez
  `libOff` (namespace per-DLL; `libOff == 0` = rozwiąż płasko po wszystkich modułach), potem załataj
  slot IAT pod `slotAddr` rozwiązanym adresem.
- **`NxExport { nameOff, addr }`** — symbol `nameOff` jest wołalny pod `addr` po załadowaniu.
- **`NxReloc { off }`** — słowo 32-bitowe pod `off` zawiera adres absolutny; loader dodaje
  deltę ładowania `(actualBase − loadBase)`.
- **`NxNeeded { nameOff }`** — załaduj `.ndl` o nazwie `nameOff`, zanim zwiążesz importy tego modułu.

---

## 3. Stos loaderów

Cztery warstwy, od build-time do runtime:

```
mknx (build, host)      ELF -> NxFormat: extract sections, collect R_386_32 -> NxReloc[],
  │                     build NxImport/NxExport/NxNeeded[], pack the string pool.
  ▼
NxeLoader (kernel, MI)  ONE image: apply relocations(+delta), bind the IAT via a resolver
  │   NxeLoader.cpp     callback, zero bss, report exports. No I/O, host-tested.
  ▼
DynLoader (kernel)      DEPENDENCY GRAPH: recursively load NEEDED .ndl, dedup, allocate
  │   DynLoader.cpp     module windows, build per-DLL symbol tables, bind across modules.
  ▼
Exec + arch usermode    PROCESS: stage the image, create an AddressSpace, map image+stack,
      Exec.cpp          iret to ring 3. (execProgram / execve / fork / exit.)
```

### 3.1 `mknx` (`tools/mknx.c`)

Natywne narzędzie hostowe (`cc -O2 -Ikernel -o bin/mknx tools/mknx.c`). Czyta ELF32 (EM_386) zlinkowany
z `-Wl,--emit-relocs`, wylicza zakresy `loadBase`/`bss`, kopiuje sekcje `SHF_ALLOC PROGBITS`
do płaskiego obrazu, zamienia zachowane relokacje `R_386_32` na `NxReloc[]`, buduje
tabele import/export/needed i zapisuje plik NxFormat. Flagi:

| Flaga | Efekt |
|---|---|
| `--dll` | ustaw `NX_FLAG_DLL` (wyjście to biblioteka). |
| `--export NAME` / `--export-all` | wstaw jeden nazwany / wszystkie symbole global+weak do `NxExport[]`. |
| `--need NAME` | dodaj bibliotekę do `NxNeeded[]` (wbudowana reguła `%.nxe` auto-dokłada `--need libc.ndl`). |
| `--implib` + `--soname NAME` | wyemituj **import library** (jeden `.s` per eksport) zamiast modułu — patrz §5. |

### 3.2 `NxeLoader` (`kernel/NxeLoader.cpp`)

Czysty, testowalny na hoście rdzeń — `loadImage(buf, cap, delta, resolve, entryOut, onExport, …)`:

1. **Relokacja** — dla każdego `NxReloc` dodaj `delta` do słowa 32-bitowego.
2. **Wiązanie importów** — dla każdego `NxImport` wywołaj `resolve(name, lib)`; przy sukcesie załataj `slotAddr`,
   przy porażce zwróć **`-2`** (głośno — nigdy cicho niezwiązany slot).
3. **Eksporty** — zrelokuj każdy `NxExport.addr` i zgłoś go przez `onExport` (tak `DynLoader`
   zbiera symbole biblioteki).
4. **Wyzeruj bss** (`bssStart..bssEnd`).

Nie robi żadnego I/O ani pracy zależnej od architektury, więc to ten sam silnik dla `.nxe`, `.ndl` i `.nkext`; różni się tylko
resolver/biblioteka.

### 3.3 `DynLoader` (`kernel/DynLoader.cpp`) — linkowanie dynamiczne

Gdy program (lub biblioteka) ma symbole needed/imported, `dynLoadProgram` rozwiązuje cały graf:

- **Ścieżka wyszukiwania** jest stała: `/disks/main/nanos/lib/<name>.ndl`.
- **Rekurencyjnie, post-order** (`ensureLib`): biblioteki `NEEDED` modułu są ładowane **przed** nim,
  więc klient `libnwui.ndl` automatycznie dostaje `libnw.ndl` → `libc.ndl` (model dyld/Windows).
- **Dedup po soname** (`LibSet`): biblioteka współdzielona w grafie (diament `libc.ndl`) ładuje się
  **raz** i jest reużywana.
- **Okna ładowania**: każda `.ndl` dostaje slot 4 MiB z **pasma modułów** (`g_nextBase`, przesuwający się
  o `arch::mmuModuleStride()` od `arch::mmuModuleBase()`), jest przepuszczana przez `NxeLoader` ze swoją
  deltą relokacji, po czym mapowana do `AddressSpace` procesu przez `arch::archLoadModule`.
- **Namespace per-DLL**: każda załadowana biblioteka ma własny `SymTable` (kluczowany soname). Import
  z nazwanym `libOff` rozwiązuje się tylko wobec tabeli tej biblioteki; `libOff == 0` rozwiązuje płasko.
  `SymTable` ma stałą pojemność (`MAX = 2048` symboli, `POOL = 32 KiB` nazw — wymiarowane pod
  ~1300 eksportów picolibc), wygrywa pierwsza definicja.

---

## 4. Mapa pamięci userland

Każdy proces ma własny `AddressSpace` (paging; patrz `arch/x86/mm/mmu_x86.cpp`). Współdzieli
połowę jądra (RAM identity-mapped, `0`–`0x8000000`) tylko do odczytu przez współdzielone PDE i posiada prywatne tablice stron
dla tych okien (wszystko zweryfikowane wobec `mmu_x86.cpp` / `usermode_x86.cpp`):

| Region | Zakres VA | Uwagi |
|---|---|---|
| **Okno użytkownika** | `0x800000`–`0xBFFFFF` | jedno PDE 4 MiB. Obraz programu pod `loadBase = 0x800000` (≈3.5 MiB max). |
| **Stos użytkownika** | `0xB80000`–`0xC00000` | 512 KiB na szczycie okna użytkownika; `esp` startuje pod `0xC00000`, rośnie w dół. |
| **Pasmo modułów** (`.ndl`) | `0x08000000`–`0x10000000` | krok 4 MiB → **32** sloty bibliotek (`NX_MOD_*`). |
| **Framebuffer** | `0x10000000` | mapowanie FB do przestrzeni użytkownika (jeśli obecny). |
| **Sterta (`brk`)** | `0x20000000`–`0x22000000` | 32 MiB, rosnąca przez `SYS_brk`/`sbrk` (`NX_BRK_*`). |
| **mmap** | `0x30000000`–`0x34000000` | 64 MiB, bump-alokowane dla `mmap(MAP_ANONYMOUS)` + file-backed (`NX_MMAP_*`). |

**Okno staging.** Zanim przestrzeń procesu zaistnieje, `execProgram`/`execve` wczytują `.nxe` do
**okna staging** `STAGE_BASE = 0x800000`, `STAGE_CAP = 4 MiB` (`kernel/Exec.cpp`) — ten sam VA
co okno użytkownika, ale dostępny pod katalogiem **jądra**, gdzie `0x800000` jest zarezerwowaną
identity-mapped ramką. Obraz jest tam relokowany/zerowany, a potem kopiowany do prywatnych
ramek procesu przez `archLoadUser`. Obraz większy niż `STAGE_CAP` jest odrzucany, zanim mógłby przepełnić bufor.

> Baza ładowania programu została podniesiona `0x400000 → 0x800000`, by dać obrazowi jądra zapas — okno
> staging, okno użytkownika i stos przesunęły się w górę zgodnie (`user/nx.ld`, `Exec.cpp`,
> `mmu_x86.cpp`, `usermode_x86.cpp` muszą się zgadzać).

---

## 5. Linkowanie z biblioteką — import library + `nx-dllimport`

Program nigdy nie linkuje samej `.ndl`; linkuje małą **import library** `lib<x>.ndl.a` zbudowaną przez
`mknx … --implib --export-all --soname <x>.ndl`. Archiwum ma jeden member per eksport:

- eksport **funkcji** → thunk `name: jmp [__imp_name]` plus slot IAT w sekcji per-biblioteka
  `.nxlib.<soname>`;
- eksport **danych** → sam slot IAT.

Linker ściąga tylko te membery, do których program faktycznie się odwołuje. `user/nx.ld` trzyma każdą
`.nxlib.<soname>` jako **osobną sekcję wyjściową**, by nazwa przetrwała linkowanie — `mknx` czyta ją, by
otagować każdy `NxImport` jego biblioteką źródłową (stąd bierze się `libOff`). Sloty żyją
wewnątrz załadowanego obrazu, by loader mógł je załatać.

**Importy danych** (`stdin`/`stdout`/`stderr`/`environ`) nie da się spełnić zwykłym
externem przez granicę modułu, więc `user/libc-glue/nx-dllimport.h` (force-included do źródeł
programu, *nie* do glue, które *jest* libc) przedefiniowuje każdy jako dereferencję jego slotu IAT —
dokładnie model `__declspec(dllimport)` z Windows:

```c
extern char ***__imp_environ;
#define environ (*__imp_environ)
```

`errno` jest wyjątkiem — to **nie** import danych. picolibc budujemy z
`-Derrno-function=__errno_location`, więc `errno` rozwija się do `(*__errno_location())` — zwykłego
importu *funkcji*, który libc.ndl rozwiązuje jak każdy symbol kodu. `__errno_location()` zwraca adres
komórki errno **bieżącego wątku** (w jego TCB pod `%gs:0`) — errno jest więc per-wątek i bez
współdzielonego slotu danych (patrz `user/libc-glue/tls.c`).

Istniejące biblioteki: **`libc.ndl`** (picolibc + warstwa syscalli `user/libc-glue`, budowana
`--export-all`), **`libnw.ndl`** (klient kompozytora NanWM), **`libnwui.ndl`** (toolkit UI, potrzebuje
libnw + libc) oraz demo **`greet.ndl`**. Biblioteki linkowane są z `user/dll.ld` (preferowana
baza `0x09000000`, relokowana przy ładowaniu).

---

## 6. Cykl życia procesu (ring 3)

### 6.1 Start

`user/crt0.S` `_start` startuje z `esp` wskazującym na `argc` (SysV i386): wyprowadza `argv`/`envp`,
publikuje środowisko (`__nx_set_environ`) i nazwę programu (`__nx_set_progname`), woła
`main(argc, argv, envp)`, potem `exit(zwrot z main)`.

### 6.2 Wejście w ring 3

`kernel/Exec.cpp` `execProgram` (oraz syscall `execve(11)`): stage obrazu, wybór
`dynLoadProgram` (jeśli cokolwiek importuje/potrzebuje) lub `loadStaged` (delta 0, brak importów), utworzenie
świeżego `AddressSpace`, potem `arch::archLoadUser` mapuje obraz i stos 512 KiB do prywatnych
ramek i buduje obraz argv/envp na stosie. Na koniec `arch::archEnterUser`
(`usermode_x86.cpp`) przełącza CR3 i robi **`iret` do ring 3** — nie wraca:

```
cli; mmuSwitch(space); ds=es=fs=gs = 0x23      ; user data, DPL 3
iret frame: ss=0x23, esp=userEsp, eflags=0x202 ; IF=1
            cs=0x1B, eip=entry                  ; user code, DPL 3
```

Selektory GDT: **kod użytkownika `0x1B`**, **dane użytkownika `0x23`** (oba DPL 3).

### 6.3 Syscalle i wyjście

Program woła jądro przez **`int 0x80`** (numery Linux i386, `kernel/SyscallNr.h`): `eax` =
numer, `ebx/ecx/edx` = argumenty, wynik w `eax` (ujemny = errno). Bramą jest wektor IDT 128
(DPL 3). `exit(1)`/`exit_group` trafia do `procExit` (`Exec.cpp`), który zwalnia przestrzeń adresową,
oznacza zadanie jako zombie i oddaje sterowanie.

> To **model trap-frame z FAZY 4**: ring 3 przez `iret`, wyjście przez syscall + scheduler. Wcześniejszy
> schemat setjmp/longjmp z powrotem do jądra zniknął (`usermode_x86.cpp`: "No longjmp").

### 6.4 fork / execve

`fork(2)` zachłannie kopiuje przestrzeń adresową (w tym okna heap/module/mmap) oraz tablicę
fd; dziecko wznawia z tego samego trap frame z `eax = 0`. `execve(2)` zastępuje obraz
**w miejscu**: stage + ładowanie do świeżej przestrzeni, potem `arch::archFrameToUser` przepisuje
trap frame zadania, które weszło w syscall (`eip`=entry, `useresp`=userEsp, `cs/ss/ds`, `eflags`), tak by końcowy
`iret` syscalla wpadł do nowego programu. Powłoka (`user/nsh.c`) rozwiązuje nazwę komendy do ścieżki (patrz
filesystem.md) i robi na niej `execve`; PID 1 (`init`) jest uruchamiany bezpośrednio przez jądro przez
`execProgram`.

### 6.5 Sygnały

Dostarczenie biegnie w kontekście samego celu (aktywne jest jego użytkownikowe CR3): `arch::archPushSignalFrame`
buduje `SigContext` plus argument handlera i adres powrotu na **stosie użytkownika**, ustawia
trap frame na handler i pozwala `iret` wejść w niego w ring 3. Trampolina libc
(`user/libc-glue/sigtramp.*`) woła `sigreturn`, a `arch::archSigreturn` przywraca zapisany
kontekst — z polityką restart/EINTR/keep zastosowaną do przerwanego syscalla.

---

## 7. Szybka referencja

| Stała | Wartość | Gdzie |
|---|---|---|
| `NX_MAGIC` / `NX_VERSION` | `0x0045584E` / 3 | `kernel/NxFormat.h` |
| `loadBase` programu | `0x800000` | `user/nx.ld`, `Exec.cpp STAGE_BASE` |
| preferowana baza biblioteki | `0x09000000` | `user/dll.ld` (relokowana przy ładowaniu) |
| okno użytkownika | `0x800000`–`0xBFFFFF` | `usermode_x86.cpp` |
| stos użytkownika | `0xB80000`–`0xC00000` (512 KiB) | `usermode_x86.cpp` |
| okno staging | `0x800000`, 4 MiB | `Exec.cpp` |
| pasmo modułów | `0x08000000`–`0x10000000`, krok 4 MiB (32) | `mmu_x86.cpp` |
| sterta (`brk`) | `0x20000000`–`0x22000000` (32 MiB) | `mmu_x86.cpp` |
| mmap | `0x30000000`–`0x34000000` (64 MiB) | `mmu_x86.cpp` |
| selektory kodu / danych użytkownika | `0x1B` / `0x23` (DPL 3) | `usermode_x86.cpp` |
| brama syscalli | `int 0x80`, IDT 128 (DPL 3) | `SyscallNr.h`, `arch/x86/cpu/Idt.cpp` |
| pojemność `SymTable` | 2048 symboli / 32 KiB nazw | `DynLoader.h` |

**Kluczowe pliki:** `kernel/NxFormat.h` (format), `tools/mknx.c` (ELF→Nx), `kernel/NxeLoader.cpp` (loader
obrazu, testowany na hoście), `kernel/DynLoader.cpp` (linker dynamiczny), `kernel/Exec.cpp` (cykl życia
procesu), `arch/x86/cpu/usermode_x86.cpp` (wejście w ring 3 + sygnały),
`arch/x86/mm/mmu_x86.cpp` (przestrzeń adresowa per-proces), `user/{crt0.S,nx.ld,dll.ld,libc-glue/}`
(runtime). Patrz writing-apps.md, by faktycznie zbudować lub sportować aplikację.
