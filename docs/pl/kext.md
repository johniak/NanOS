# Moduły jądra NanOS (kext / `.nkext`)

**kext** ("nkext", Nano Kernel EXTension) to sterownik urządzenia, który **nie** jest wkompilowany w
`kernel.bin` — dostarczany jest jako osobny plik w `/nanos/kext/`, a jądro **ładuje go podczas
rozruchu**. Działa w ring 0, w przestrzeni adresowej jądra, z bezpośrednim dostępem do sprzętu, ale
komunikuje się z jądrem wyłącznie przez małe, stabilne C ABI **z importem po nazwie** — ten sam
model dynamicznego linkowania w stylu Windows PE, którego NanOS używa dla przestrzeni użytkownika
`.nxe`/`.ndl` (zobacz filesystem.md, gdzie żyją moduły, oraz networking.md, gdzie kext e1000
przedstawiony jest w kontekście).

> Porównanie z Linuksem: to idea ładowalnych modułów z Linuksa (`insmod`/`modprobe`, `EXPORT_SYMBOL`,
> sterowniki jako `.ko` poza `vmlinuz`) — ale minimalna: jedna ustalona lista eksportów, ładowanie
> tylko przy rozruchu (brak `modprobe`/rozwiązywania zależności, na razie brak `rmmod`) oraz ten sam
> format binarny co przestrzeń użytkownika.

---

## 1. Po co istnieją kexty

- **Utrzymanie małego obrazu jądra.** Sterowniki klawiatury PS/2, myszy PS/2 i karty sieciowej e1000
  **nie** są w `kernel.bin` — to trzy pliki `.nkext`. `kernel.bin` niesie tylko rdzeń
  (CPU/MM/VFS/rdzeń sieci/scheduler) oraz loader modułów.
- **Sterownik jest dostarczalną, identyfikowalną jednostką.** Dodanie obsługi sprzętu = wrzucenie
  kolejnego `.nkext` do `/nanos/kext/`. Haki, które jądro już posiada (`DeviceManager`,
  `Vfs::registerType`, rejestr urządzeń sieciowych, tablica IRQ), to punkty rejestracji, których
  używa moduł.
- **Jeden format binarny dla wszystkiego.** Programy (`.nxe`), biblioteki współdzielone (`.ndl`) i
  moduły jądra (`.nkext`) to wszystko **NxFormat** — ten sam silnik loadera, ta sama relokacja, to
  samo wiązanie importu po nazwie. kext to w istocie `.ndl`, którego importy rozwiązują się do tablicy
  eksportów *jądra* zamiast `libc.ndl`.

---

## 2. Format binarny

`.nkext` to moduł **NxFormat** (`kernel/NxFormat.h`, magic `"NXE"` = `0x0045584E`,
`NX_VERSION 3`). To model w stylu Windows PE: linkowany pod *preferowaną* bazą, ale ładowalny
gdziekolwiek — loader dodaje deltę ładowania `(actualBase − preferredBase)` do każdego adresu w
**tablicy relokacji** (poprawki bezwzględne R_386_32) i łata każdy **import** po nazwie.

Pola `NxHeader`, których używa kext:

| Pole | Znaczenie dla kextu |
|---|---|
| `entry` | punkt wejścia modułu — **`nkext_init`** (`ENTRY` w `kext/kext.ld`). |
| `loadBase` | preferowana baza **`0xD0000000`** — dowolna; wszystko jest relokowane przy ładowaniu. |
| `importTable` / `importCount` | `NxImport[]` — symbole jądra, których potrzebuje moduł, w zakresie biblioteki `"kernel"`. |
| `relocTable` / `relocCount` | `NxReloc[]` — poprawki R_386_32, stosowane z deltą ładowania. |
| `bssStart` / `bssEnd` | zerowane przez loader. |
| `exportTable` / `exportCount` | nieużywane przez obecne kexty (moduł *mógłby* eksportować, ale żaden nie eksportuje). |

Układ na dysku jest standardowym układem NxFormat:
`[ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] | NxNeeded[] | strings ]`
(`bss` nie jest przechowywane).

---

## 3. Ładowanie (podczas rozruchu)

`kernel/Kernel.cpp` (`Kernel::start`) wywołuje **`loadAllKexts(vfs, "/disks/main/nanos/kext")`**
po enumeracji PCI (aby sterownik karty sieciowej mógł znaleźć swoje urządzenie) i przed
uruchomieniem interfejsu syscall i schedulera:

```
PCI bus enumeration → loadAllKexts(/nanos/kext) → syscall interface → scheduler/init
```

`kernel/KextLoader.cpp`:

- **`loadAllKexts(vfs, dir)`** — wykonuje `readdir` katalogu, filtruje nazwy kończące się na `.nkext` i
  dla każdej wywołuje `loadKext`, wypisując `kext: <name> [loaded]` / `[FAILED]`. Zwraca liczbę
  załadowanych.
- **`loadKext(vfs, path)`** —
  1. `stat` + podejrzenie `NxHeader`, by ustalić rozmiar bufora (musi pomieścić przechowywany obraz
     **oraz** jego bss: `bssEnd − loadBase`, zaokrąglone w górę do strony).
  2. Alokuje pamięć jądra **wyrównaną do strony** (`new` + wyrównanie w górę do `0x1000`). Jest
     mapowana tożsamościowo i wykonywalna (i686, brak bitu NX), więc zrelokowany kod **wykonuje się w
     miejscu w ring 0**.
  3. Wczytuje plik, następnie `loadKextImage` → `NxeLoader::loadImage` z
     `delta = buffer − loadBase`, resolver = **`kernelResolveSym`** — stosuje relokacje, wiąże
     importy, zeruje bss.
  4. Zapisuje moduł w rezydentnym rejestrze `g_mods[]` (maks. **16**, `MAXKEXT`). Bufor obrazu jest
     **trzymany, nigdy nie zwalniany** — podstawa pod przyszły `rmmod`/wyładowanie.
  5. Wywołuje punkt wejścia: `int (*)()` w `nkext_init`, **w ring 0**, który rejestruje obsługę IRQ i
     urządzenie(-a) modułu.

**Błędy są głośne, nigdy ciche:** import, którego jądro nie eksportuje, powoduje, że `NxeLoader`
zwraca `-2`, `loadKext` zwalnia bufor i zgłasza `[FAILED]`.

Współdzielony silnik loadera to `kernel/NxeLoader.cpp` (`loadImage`) — identyczny dla `.nxe`, `.ndl`
i `.nkext`; jedyną różnicą jest przekazany resolver/biblioteka.

---

## 4. ABI jądro↔kext

Jądro eksportuje ustalony zbiór funkcji `extern "C"` do modułów — `EXPORT_SYMBOL` w wydaniu NanOS.
**Jedynym źródłem prawdy** jest `kernel/kexports.def`, lista X-makr (`KX(name)`); rozwijają ją dwaj
konsumenci:

- `kernel/KernelExports.cpp` buduje runtime'ową tablicę resolvera; **`kernelResolveSym(name, lib)`**
  wyszukuje nazwę i zwraca jej adres (lub 0 → import nie wiąże się). Wszystkie symbole jądra dzielą
  niejawną przestrzeń nazw biblioteki `"kernel"`.
- Makefile (awk) generuje **zaślepkę importu** kextu (sloty `__imp_<name>` + thunki).

`kernelExportsInit(SynthFs* root)` jest wywoływane raz podczas rozruchu (`Kernel.cpp`), aby
`knx_add_input_dev()` mogło zarejestrować węzły `/dev/input<N>` w syntetycznym korzeniu.

Eksportowane symbole (`kexports.def`):

| Kategoria | Symbole |
|---|---|
| **Pamięć** | `knx_malloc`, `knx_free` (sterta jądra) |
| **Logowanie** | `knx_log(const char*)` (konsola) |
| **Czas** | `knx_uptime_us()` |
| **Przerwania** | `knx_register_irq(int irq, void (*h)(void*))` |
| **Wejście** | `knx_feed_scancode(unsigned char)`, `knx_add_input_dev(CharDevice*)` → `/dev/input<N>` |
| **PCI** | `knx_pci_find`, `knx_pci_bar`, `knx_pci_bar_size`, `knx_pci_bar_is_io`, `knx_pci_irq`, `knx_pci_enable_bus_master`, `knx_pci_cfg_read32`, `knx_pci_cfg_write32` |
| **Sieć / DMA / MMIO** | `knx_map_mmio`, `knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx` (ABI sieci, `kernel/knx_net.h`) |

ABI sieci (`kernel/knx_net.h`) jest celowo **nieprzezroczyste**: sterownik wypełnia płaską
`KnxNetDev { name, mac, mtu, tx, drvctx }` i wymienia *surowe bajty ramek* — nigdy nie widzi
wewnętrznych `NetDevice`/`NetBuf` jądra, więc układ nie może się rozjechać. `knx_netif_rx` można
bezpiecznie wywoływać z obsługi IRQ sterownika (tylko kolejkuje + budzi wątek softirq; zobacz
networking.md §2.2).

> Porównanie z Linuksem: jedna płaska lista eksportów, brak wersjonowania symboli (`modversions`),
> brak symboli tylko-GPL, brak przestrzeni nazw per-moduł. Importy są po łańcuchu nazwy, wiązane przy
> ładowaniu — nie przez przekazaną vtable.

---

## 5. Istniejące moduły

Wszystkie trzy ładują się podczas rozruchu z `/nanos/kext/` (`KEXTS = kbd mouse e1000` w Makefile):

| Moduł | Źródło | Wiąże | Rejestruje | Używa |
|---|---|---|---|---|
| **`kbd.nkext`** | `kext/kbd/kbd_ps2.cpp` | PS/2 8042, **IRQ1** | dostarcza skankody do evdev/konsoli jądra | `knx_register_irq`, `knx_feed_scancode`, `knx_log` |
| **`mouse.nkext`** | `kext/mouse/mouse_ps2.cpp` (+ `MouseDevice.cpp`) | PS/2 AUX, **IRQ12** | `CharDevice` → `/dev/input<N>` | `knx_register_irq`, `knx_add_input_dev`, … |
| **`e1000.nkext`** | `kext/e1000/e1000.cpp` | PCI Intel 82540EM (**`8086:100E`**) | urządzenie sieciowe `eth0` (pierścienie DMA RX/TX po 32) | `knx_pci_*`, `knx_map_mmio`, `knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx` |

`nkext_init()` każdego modułu wykonuje rozruch sprzętu i rejestrację, a następnie zwraca 0.
Dyscyplina linii klawiatury oraz *polityka* `/dev/input0` pozostają w jądrze — kext jest właścicielem
tylko sprzętu (odczytaj skankod na IRQ1, przekaż go dalej).

### Haki rejestracji runtime'owej

| Aby zarejestrować… | Wywołanie | Przykład |
|---|---|---|
| obsługę IRQ | `knx_register_irq(irq, fn)` | `kbd`: `knx_register_irq(1, kbdIrq)` |
| urządzenie wejściowe (`/dev/input<N>`) | `knx_add_input_dev(dev)` | `mouse` |
| urządzenie sieciowe (`eth0`) | `knx_add_net_dev(&desc)` | `e1000` |
| znajdź / skonfiguruj urządzenie PCI | `knx_pci_find` + `knx_pci_bar` + `knx_pci_enable_bus_master` | `e1000` |
| zmapuj rejestry urządzenia (MMIO) | `knx_map_mmio(phys, len)` | `e1000` BAR0 |
| zaalokuj pamięć DMA | `knx_dma_alloc(len, &phys)` | pierścienie `e1000` |

---

## 6. Budowanie kextu

Budowa kextu jest **oddzielna** od `kernel.bin` (obiekty kextu nigdy nie są w jądrowym `SOURCES`):

- **Kompilacja** (`KEXT_CFLAGS`): freestanding C++ dla ring 0 —
  `-ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti -fno-pic …`, include'y
  `-Iarch/include -Ikernel -Idrivers -Iinclude`. Celowo **bez `-Ilib`** (pułapka
  case-insensitivity macOS) i bez picolibc.
- **Zaślepka importu jądra** `kimports.S` jest generowana z `kexports.def` przez awk: slot `__imp_<name>: dd
  0` na każdy eksport w sekcji `.nxlib.kernel`, plus thunk `<name>: jmp [__imp_<name>]` w
  `.text`. `mknx` później oznacza te importy biblioteką źródłową `"kernel"`.
- **Klej runtime'owy** `KEXT_GLUE = nxhdr.o + kimports.o + kext_rt.o`. `kext/kext_rt.cpp` to maleńki
  runtime C/C++ (`memset`/`memcpy`, `operator new`/`delete` delegujące do `knx_malloc`/`knx_free`).
- **Linkowanie** z `kext/kext.ld` (`ENTRY(nkext_init)`, baza `0xD0000000`, sekcje wraz z
  `.nxheader`/`.nxlib.kernel`) z użyciem `-Wl,--emit-relocs`, aby poprawki R_386_32 przetrwały.
- **`mknx`** (`tools/mknx.c`, narzędzie hostowe) konwertuje zlinkowany ELF → `.nkext`: wyciąga sekcje,
  buduje `NxReloc[]` z relokacji oraz `NxImport[]` z symboli `__imp_<name>`.
- **Instalacja**: cel `_image` wykonuje `debugfs write` każdego `bin/<m>.nkext` do `/nanos/kext/<m>.nkext`
  w obrazie dysku, gdzie znajduje je `loadAllKexts`.

### Dodawanie nowego kextu

1. Napisz `kext/<name>/<name>.cpp` z `extern "C" int nkext_init()` oraz deklaracjami `extern "C"`
   dla symboli `knx_*`, których używasz.
2. Dodaj regułę linkowania per-kext w Makefile (skopiuj istniejącą) i dopisz `<name>` do `KEXTS`.
3. Jeśli potrzebujesz funkcji jądra, która nie jest jeszcze eksportowana, dodaj linię `KX(knx_…)` do
   `kernel/kexports.def` i zaimplementuj ją w `kernel/KernelExports.cpp` (+ deklarację w
   odpowiednim `knx_*.h`). Zarówno resolver, jak i zaślepka importu regenerują się z tej jednej linii.

---

## 7. Ring i przywileje

kext działa w **ring 0**, w **przestrzeni adresowej jądra** (mapowanej tożsamościowo, wykonywalnej —
i686 nie ma bitu NX), z **bezpośrednim dostępem do sprzętu**: port I/O (inline `inb`/`outb`), MMIO, DMA i
rejestracja IRQ. **Nie ma izolacji** od jądra — wadliwy kext może uszkodzić system,
dokładnie jak moduł Linuksa.

> Porównanie z przestrzenią użytkownika `.nxe`: te działają w **ring 3**, w izolowanej przestrzeni
> adresowej, i docierają do jądra tylko przez syscalle `int 0x80`. kext to przeciwny biegun — pełny
> przywilej, ten sam format binarny.

---

## 8. Testowanie

`tests/test_kextloader.cpp` ćwiczy loader na hoście (bez QEMU): relokację z niezerową
deltą, wiązanie importu przez atrapowy resolver, zakresowanie biblioteki `"kernel"` oraz ścieżkę
głośnego błędu dla nierozwiązanego importu. Trzy prawdziwe moduły są weryfikowane w QEMU (wejście z
klawiatury, mysz `/dev/input` oraz `ping`/`wget` przez e1000 — zobacz networking.md §12).

---

## 9. Ograniczenia i prace na przyszłość

| | Obecnie |
|---|---|
| Moduły rezydentne | **16** (`MAXKEXT`) |
| Ładowanie | **tylko podczas rozruchu**, całość `/nanos/kext/*.nkext` |
| Wyładowanie | **na razie żadne** — obrazy modułów pozostają rezydentne (rejestr to hak pod `rmmod`) |
| Zależności | **żadne** — brak zależności/auto-ładowania w stylu `modprobe` |
| Eksporty z kextu | format to wspiera; żaden obecny moduł nie eksportuje |
| Izolacja | **żadna** (ring 0, współdzielona przestrzeń adresowa) |

Na przyszłość: `rmmod`/wyładowanie (rejestr `g_mods[]` + trzymane bufory już istnieją na ten cel),
ładowanie na żądanie (`modprobe` po identyfikatorze urządzenia) oraz więcej sterowników (urządzenia
blokowe, inne karty sieciowe) przez to samo ABI — dodanie sterownika to dodanie `.nkext`, a nie
edycja `kernel.bin`.
