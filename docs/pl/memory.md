# Zarządzanie pamięcią w NanOS

NanOS działa z **włączonym pagingiem** i **przestrzenią adresową per-proces** — a nie z płaskim
modelem bez pagingu, który opisują wczesne notatki w CLAUDE.md. Na stos składają się: fizyczny **frame allocator**
(bitmapa), bring-up **pagingu x86** (kernel mapowany identycznościowo + prywatne okna użytkownika), prawdziwy
**kernel heap** (free-list z łączeniem, za `new`/`malloc`) oraz okna userlandu
(`brk`, `mmap`, pasmo bibliotek współdzielonych). Ten dokument to scala; mapa VA userlandu pojawia się też
z perspektywy programu w nxe-ndl.md §4 oraz z perspektywy per-proces w scheduler.md §5.

---

## 1. Fizyczny frame allocator (`mm/FrameAllocator.*`)

**Bitmapa** nad ramkami 4 KiB (`FRAME_SIZE = 4096`), jeden bit na ramkę, `MAX_FRAMES = 1<<20` →
bitmapa 128 KiB mieszkająca w `.bss` (bez konstruktora — globalna `g_frames` jest zerowana przez loader).

- `init(topOfRam)` oznacza wszystko jako używane, a następnie `arch::bootMemForEachUsable` (mapa Multiboot,
  boot.md §3) woła `markFree` na każdym użytecznym zakresie. Zakresy zarezerwowane pozostają używane: niska pamięć
  (<1 MiB), obraz jądra, okno staging 4 MiB pod `0x800000` oraz kernel heap.
- `alloc()` zwraca fizyczny adres wolnej ramki (albo 0 = OOM); `free(pa)` czyści jej bit;
  `freeCount()` przechodzi po bitmapie na potrzeby `/proc/meminfo`.
- Czysty → testowany na hoście (`test_frameallocator.cpp`).

To jedyne źródło fizycznych stron — zarówno page tables, jak i ramki danych użytkownika pochodzą stąd.

---

## 2. Bring-up pagingu (`arch/x86/mm/mmu_x86.cpp`, `Paging.h`, `PagingControl.h`)

Klasyczny 32-bitowy paging x86 dwupoziomowy (10-bit indeks PD | 10-bit indeks PT | 12-bit offset), flagi PTE
`PRESENT | RW | USER`. Wrappery rejestrów CR są w `PagingControl.h` (`loadCr3`/`readCr3`/
`enablePaging`/`readCr2`).

`mmuInitKernel(frames, topOfRam)`:
1. rezerwuje regiony ramek (niska pamięć, kernel, staging, heap);
2. układa arenę kernel heapu (`heapInit`, §5) — **zanim** paging jest włączony, gdy adresy są
   fizyczne;
3. buduje page directory jądra i **mapuje identycznościowo cały RAM** `[0, topOfRam)` jako `PRESENT|RW`;
4. `cli`, `loadCr3(kernelDir)`, `enablePaging()` (ustaw CR0.PG), `sti`.

`mmuMapKernelMmio(phys, len)` mapuje MMIO urządzeń do żywego page directory jądra — używane przy starcie do mapowania
framebuffera, zanim istnieje jakakolwiek przestrzeń per-proces, więc mapowanie jest współdzielone (terminal.md).

---

## 3. Przestrzeń adresowa per-proces (`arch/x86/mm/AddressSpace.*` + `mmu_x86.cpp`)

Każdy proces dostaje własne page directory, **sklonowane z page directory jądra**, tak by cały
identycznościowo mapowany RAM i kernel heap były współdzielone; następnie konkretne okna są wykrawane jako
**prywatne** page tables:

- `mmuCreateAddressSpace()` → `adoptKernelDirectory(kernelDir, 0x800000)`: kopiuje każde PDE z
  page directory jądra, następnie **zeruje PDE pokrywające okno użytkownika** (`0x800000`), tak by pierwsze mapowanie
  tam alokowało prywatny page table zamiast aliasować ramkę staging.
- `AddressSpace::map(va, pa, flags)`: na żądanie — jeśli PDE nie jest obecne, alokuje wyzerowaną ramkę
  na nowy page table (`PRESENT|RW|USER`), a następnie ustawia PTE.
- `mmuSwitch(space)` ładuje directory do CR3 (flush TLB); scheduler woła to przy każdym
  context switch (scheduler.md §2).
- `mmuCopyAddressSpace(parent)` (**fork**, zachłanny): klonuje połowę jądra, następnie `copyUserWindowFrom`
  dla każdego wypełnionego prywatnego okna (okno użytkownika, heap, pasmo modułów, mmap) — świeże ramki, skopiowane bajty.
- `mmuFreeAddressSpace(space)` (exit): zwalnia tylko prywatne części (PT okien + ich ramki, samo
  directory); współdzielone PDE jądra oraz okno MMIO framebuffera pozostają nietknięte.
- `AddressSpace` jest czysty → testowany na hoście (`test_addressspace.cpp`).

---

## 4. Pełna mapa pamięci

Stałe z `mmu_x86.cpp`. Poniżej `0x10000000` RAM jest mapowany identycznościowo w directory
jądra; proces nadpisuje konkretne okna prywatnymi mapowaniami.

| Region | Zakres VA | Stała / uwaga |
|---|---|---|
| Niska pamięć + VGA | `0` – `0x100000` | zarezerwowane |
| Obraz jądra | `0x100000`+ | baza ładowania (`linker.ld`) |
| RAM mapowany identycznościowo | `0` – `topOfRam` | directory jądra (wraz z kernel heapem) |
| **Okno staging Exec** | `0x800000`, 4 MiB | zarezerwowana ramka; obraz stage'owany pod CR3 jądra (nxe-ndl.md §4) |
| **Okno użytkownika** (prywatne) | `0x800000` – `0xBFFFFF` | obraz programu + 512 KiB stosu na szczycie |
| **Pasmo modułów** (prywatne) | `0x08000000` – `0x10000000` | `NX_MOD_BASE/STRIDE/MAX` — 4 MiB × 32 sloty `.ndl` |
| **Framebuffer** | `0x10000000` | `FB_USER_VA` — MMIO, nie zwalniane przy exit |
| **Heap / brk** (prywatne) | `0x20000000` – `0x22000000` | `NX_BRK_BASE/MAX` — 32 MiB |
| **mmap** (prywatne) | `0x30000000` – `0x34000000` | `NX_MMAP_BASE/MAX` — 64 MiB |

---

## 5. Kernel heap (`mm/Heap.*` + `mm/memory_manager.*`)

Alokator jądra za `new`/`malloc` to **prawdziwy heap typu free-list z boundary tags i
łączeniem** (`mm/Heap.cpp`) — a nie bump allocator, i `free` naprawdę zwalnia. (Stary
bump allocator z `free` będącym no-op, wspominany we wczesnych notatkach CLAUDE.md, został zastąpiony.)

- Każdy blok ma 8-bajtowy nagłówek + 8-bajtową stopkę (rozmiar + bit used); free list jest podwójnie
  wiązana przez **offsety od bazy areny** (więc jest testowalna na hoście 64-bitowym).
- `alloc` to first-fit + podział; `free` łączy obu sąsiadów.
- **Integralność:** kanarek `0xC7` po każdej alokacji + sprawdzenia boundary-tag; niezgodność woła
  `onCorruption` → kernel panic (to właśnie zamienia przepełnienie stosu jądra danego zadania w
  czysty panic, a nie cichą korupcję — zob. scheduler.md §1).
- `memory_manager.cpp` to cienka fasada: `heapInit(base,size)` układa arenę (wołane z
  `mmuInitKernel`), a `malloc/calloc/realloc/free` + `operator new/delete` z linkowaniem C++
  delegują do heapu. `heapTotalBytes()`/`heapFreeBytes()` zasilają `/proc/meminfo`.
- Testowany na hoście (`test_memory_manager.cpp`).

> Zwróć uwagę na **linkowanie C++** `malloc` itd. (a nie `extern "C"`) — testy hostowe dostarczają pasujące
> shimy przekazujące do libc, ponieważ nie linkują `memory_manager` (CLAUDE.md, Testing).

---

## 6. User heap i mmap (`brk` / `mmap`)

Okna heapu i mmap userlandu rosną na żądanie przez syscalle (syscalls.md), każde
implementowane przez przełączenie na directory jądra (jedyne miejsce, gdzie świeża ramka jest
identycznościowo dostępna do wyzerowania), zmapowanie ramek do przestrzeni *procesu*, a następnie odtworzenie CR3:

- **`brk`/`sbrk`** → `mmuSetUserBrk(space, old, new)`: zaokrągla oba końce do strony; rośnięcie mapuje świeże
  wyzerowane ramki `USER|RW` w `[old, new)`, kurczenie unmapuje + zwalnia. W stylu glibc: zwraca nowy
  break, niezmieniony przy porażce. Okno: `NX_BRK_BASE … NX_BRK_MAX`.
- **`mmap`** → `mmuMapAnon(space, base, bytes, writable)` dla `MAP_ANONYMOUS` (świeże wyzerowane
  ramki) oraz jako magazyn dla map opartych o plik (dispatch następnie wypełnia je z pliku);
  `mmuMapUserFb` mapuje MMIO framebuffera pod `FB_USER_VA`. Okno: `NX_MMAP_BASE … NX_MMAP_MAX`.

Oba okna są duplikowane przy fork (`copyUserWindowFrom`) i zwalniane przy exit
(`mmuFreeAddressSpace`).

---

## 7. `/proc/meminfo`

Renderowane przez `fs/SynthFs.cpp` ze statystyk jądra (`Kernel.cpp`):

- `MemTotal` = `bootMemTop()/1024`, `MemFree` = `g_frames.freeCount() * 4`,
- `KHeapTotal`/`KHeapFree` = rozmiar areny heapu / wolny payload.

---

## 8. Niezmienniki

- **Brak globalnych konstruktorów** — opieramy się na wyzerowanym `.bss` + jawnym `init()` (bitmapa
  frame allocatora, arena heapu). `.bss` jest zerowane przez loader Multiboot.
- **Paging jest włączony** (identycznościowa mapa jądra + okna per-proces) — linijka o „pojedynczej płaskiej przestrzeni adresowej"
  w starych notatkach CLAUDE.md jest historyczna.
- **Pojedynczy CPU**, brak TLB shootdown poza niejawnym flushem przy `loadCr3`.

**Kluczowe pliki:** `mm/{FrameAllocator,Heap,memory_manager}.*`, `arch/x86/mm/{mmu_x86.cpp,
AddressSpace.*, Paging.h, PagingControl.h}`, `kernel/{Kernel.cpp, SyscallDispatch.cpp}`,
`fs/SynthFs.cpp`.
