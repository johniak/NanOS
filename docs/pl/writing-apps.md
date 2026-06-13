# Pisanie aplikacji dla NanOS

Kompletny przewodnik dla osoby, która chce stworzyć nową aplikację działającą na NanOS —
od „hello world" po port realnej linuksowej aplikacji (jak `wget` czy `ping`).

Dokument opisuje **dwie niezależne ścieżki** i obie maszynerie, które za nimi stoją:

1. **Aplikacja wbudowana (in-tree)** — kod w `user/*.c`, budowany przez `Makefile` NanOS
   przy użyciu kontenerowego picolibc. Tak powstają `nsh`, `cat`, `ls`, `free`, kompozytor
   `nwm`, Doom itd.
2. **Port zewnętrznej aplikacji** — istniejący program (GNU/BSD) budowany przez osobny
   cross-toolchain **nanos-sdk** (`i686-nanos`) sterowany manifestem `nxport.toml`. Tak
   powstają `wget`, `ping`, `vim`, `grep`, `inetd`, `darkhttpd`.

Wspólny mianownik obu ścieżek to format wykonywalny **`.nxe`** i model linkowania
dynamicznego import-by-name (`.ndl`). Zaczynamy więc od niego — zrozumienie formatu sprawia,
że reszta jest oczywista.

> **TL;DR**
> - Chcesz dopisać własny mały program do systemu → **Ścieżka A** (sekcja 3).
> - Chcesz napisać aplikację **okienkową** (GUI/NanWM) → **sekcja 3.8**.
> - Chcesz uruchomić istniejący linuksowy program → **Ścieżka B** (sekcja 4).
> - Chcesz zrozumieć, jak to w ogóle działa pod spodem → sekcje 2, 5, 6 (+ `windowing.md` dla GUI).

---

## Spis treści

1. [Model wykonywalny w pigułce](#1-model-wykonywalny-w-pigułce)
2. [Toolchain — skąd się bierze kompilator](#2-toolchain--skąd-się-bierze-kompilator)
3. [Ścieżka A: aplikacja wbudowana (`user/*.c`)](#3-ścieżka-a-aplikacja-wbudowana-userc)
4. [Ścieżka B: port zewnętrznej aplikacji (nanos-sdk)](#4-ścieżka-b-port-zewnętrznej-aplikacji-nanos-sdk)
5. [Format `.nxe`/`.ndl` w szczegółach](#5-format-nxendl-w-szczegółach)
6. [Jak kernel ładuje i uruchamia program](#6-jak-kernel-ładuje-i-uruchamia-program)
7. [Gdzie aplikacja ląduje na dysku](#7-gdzie-aplikacja-ląduje-na-dysku)
8. [API dostępne dla aplikacji (libc + syscalle)](#8-api-dostępne-dla-aplikacji-libc--syscalle)
9. [Debugowanie i typowe problemy](#9-debugowanie-i-typowe-problemy)
10. [Checklisty](#10-checklisty)
11. [Mapa plików referencyjnych](#11-mapa-plików-referencyjnych)

---

## 1. Model wykonywalny w pigułce

NanOS **nie używa ELF-a w runtime**. Program kompiluje się normalnie do ELF-a, po czym
narzędzie **`mknx`** konwertuje go do własnego formatu `.nxe` (Nano eXecutable). Bibliotekę
dzieloną zapisuje się jako `.ndl` (Nano Dynamic Library). To jeden format z trzema wariantami:

| Rozszerzenie | Co to jest                | Flaga |
|--------------|---------------------------|-------|
| `.nxe`       | Program (executable)      | —     |
| `.ndl`       | Biblioteka dzielona       | `NX_FLAG_DLL` |
| `.nkext`     | Moduł jądra (ring 0; patrz `kext.md`) | — |

Model linkowania jest **w stylu Windows PE / MinGW**, nie ELF/glibc:

- Program **importuje funkcje po nazwie** z nazwanych bibliotek (`libc.ndl`). Loader łata
  każdy slot IAT (Import Address Table) adresem rozwiązanym w bibliotece.
- Biblioteka **eksportuje funkcje po nazwie**.
- Moduł jest linkowany pod „preferowaną" bazą, ale może załadować się gdziekolwiek —
  loader dodaje deltę `(realna_baza − preferowana_baza)` do każdego adresu z tabeli
  relokacji (`R_386_32`).

Konsekwencja jest kluczowa: **binarka zależy od stabilnego, nazwanego API, a nie od konkretnej
wersji biblioteki wkompilowanej na sztywno.** Jedna binarka działa na każdym buildzie, który
eksportuje to API — brak problemu „binarka na każdą dystrybucję".

Layout pliku `.nxe` (i obrazu w pamięci, gdy załadowany pod `loadBase`):

```
[ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] | NxNeeded[] | strings | (bss — nie zapisywane) ]
```

Szczegóły struktur → sekcja 5.

---

## 2. Toolchain — skąd się bierze kompilator

NanOS jest projektem **cross-kompilowanym**: host to macOS, ale kod celuje w i686 (32-bit
protected mode). Cały build dzieje się w kontenerach Docker; natywnie na macOS uruchamia się
tylko QEMU.

Są **dwa różne kompilatory** dla dwóch ścieżek:

### Ścieżka A — kontenerowy `i686-elf` + picolibc

- Obraz `nanos-build` (`docker/Dockerfile`) buduje toolchain `i686-elf` ze źródeł
  (binutils + gcc) oraz **picolibc** zainstalowane w `/opt/picolibc/i686-elf`.
- To tym kompilatorem `Makefile` buduje wszystkie wbudowane programy z `user/*.c`.
- Cross-prefix to zmienna `$(CROSS)`, picolibc to `PICOLIBC=/opt/picolibc/i686-elf`.

### Ścieżka B — natywny cross-target `i686-nanos` (nanos-sdk)

- Osobne repo **`nanos-sdk`** (`git@github.com:johniak/nanos-sdk.git`, lokalnie w
  `~/Projects/nanos-sdk`) buduje **prawdziwy cross-target** `i686-nanos`: spatchowane
  binutils + gcc, które rozumieją system docelowy `nanos`.
- `i686-nanos-gcc -dumpmachine` → `i686-nanos`; zdefiniowane są `__nanos__` / `__NanOS__`;
  `./configure --host=i686-nanos` działa out-of-the-box.
- To jest toolchain do **portowania istniejących aplikacji** (autotools/cmake/meson/make).

Most między nimi: NanOS jest źródłem prawdy dla nagłówków i `libc.ndl`. Sysroot SDK
odświeża się z buildu NanOS (skrypt `sync-sysroot`, a w praktyce targety `make ping`/`wget`/…
kopiują `libc.ndl{,.a}` + nagłówki do toolchaina przed buildem portu — sekcja 4.6).

---

## 3. Ścieżka A: aplikacja wbudowana (`user/*.c`)

Najprostszy sposób na nowy program w NanOS. Piszesz `user/myapp.c`, dopisujesz kilka linii do
`Makefile`, robisz `make image`, i `myapp` jest na dysku.

### 3.1 Minimalny przykład krok po kroku

**Krok 1 — napisz program.** Plik `user/hello.c`:

```c
#include <stdio.h>

int main(int argc, char** argv) {
	printf("hello from NanOS\n");
	if (argc > 1)
		printf("first arg: %s\n", argv[1]);
	return 0;
}
```

To zwykły C z picolibc. `printf`, `open`, `read`, `malloc`, `getenv` — wszystko działa, bo
zostanie zaimportowane z `libc.ndl` w runtime.

**Krok 2 — zarejestruj program w `Makefile`.** Dopisz nazwę do listy programów. W sekcji
userland (`USER_PROGS`) dodaj `hello`, a do `SYS_PROGS` (jeśli to narzędzie systemowe,
ląduje w `/nanos/bin`) **albo** do `APP_PROGS` (jeśli to „aplikacja", ląduje w bundlu
`/apps/<nazwa>`):

```makefile
# było:
USER_PROGS=init nsh cat ls ... dhcpcfg
# dodaj hello:
USER_PROGS=init nsh cat ls ... dhcpcfg hello

# i np. jako narzędzie systemowe:
SYS_PROGS=nsh cat ls free ... hello
```

**Krok 3 — dodaj regułę linkowania.** Generyczna reguła `%.o: user/%.c` skompiluje
`hello.c` automatycznie, ale linkowanie `.nxe` wymaga podania, z jakich obiektów składa się
program. Dla prostego programu to tylko glue startowy + własny `.o`:

```makefile
$(BINFOLDER)hello.nxe: $(USER_GLUE) $(BINFOLDER)hello.o $(BINFOLDER)libc.ndl.a
```

gdzie `$(USER_GLUE)` to wspólny startup (`crt0.o sigtramp.o nxhdr.o syscalls.o cwd.o`), a
`libc.ndl.a` to **import library** (thunki do funkcji libc). Reguła wzorcowa `%.nxe` zrobi
resztę (patrz niżej).

**Krok 4 — zbuduj obraz i uruchom:**

```sh
make image        # kompiluje userland, buduje obraz ext4, wpisuje pliki
make run          # bootuje w QEMU
```

W systemie: `hello` (przez link farm `/bin`) albo pełną ścieżką.

### 3.2 Co dokładnie robi build (flagi, reguły)

Wszystko poniżej to realne fragmenty `Makefile`.

**Ścieżki i flagi kompilacji:**

```makefile
PICOLIBC=/opt/picolibc/i686-elf

USER_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel \
  -Iuser -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term \
  -Iuser/libc-glue/include -I$(SBASE) -D_DEFAULT_SOURCE \
  -include user/libc-glue/compat-decls.h -Wall -fno-pic -fno-stack-protector $(UOPTFLAGS)

USER_LIBS=-L$(PICOLIBC)/lib -lc -lgcc
```

- `-isystem $(PICOLIBC)/include` — nagłówki picolibc.
- `-iquote kernel` — `SyscallNr.h` (numery syscalli, wspólne z jądrem).
- `-Iuser/libc-glue/include` — POSIX-owe nagłówki NanOS (dirent, netinet, sys/* itd.).
- `-include user/libc-glue/compat-decls.h` — wymuszone deklaracje brakujące w picolibc.
- `-fno-pic -fno-stack-protector` — stała baza, brak stack-protectora.
- `$(UOPTFLAGS)` = `-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks`.

**Wspólny startup (linkowany do każdego programu):**

```makefile
USER_GLUE=$(BINFOLDER)crt0.o $(BINFOLDER)sigtramp.o $(BINFOLDER)nxhdr.o \
          $(BINFOLDER)syscalls.o $(BINFOLDER)cwd.o
```

**Reguła kompilacji** — programy z `user/*.c` dostają wymuszony nagłówek `nx-dllimport.h`
(przekierowuje symbole-dane jak `stdout`/`errno` przez sloty IAT — patrz 3.5):

```makefile
DYNHDR=-include user/libc-glue/nx-dllimport.h

$(BINFOLDER)%.o: user/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
```

(Kod glue z `user/libc-glue/*.c` jest kompilowany **bez** `DYNHDR`, bo to on *jest* libc.)

**Reguła linkowania `.nxe`:**

```makefile
$(BINFOLDER)%.nxe: $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(@:.nxe=.elf) \
	  $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(MKNX) $(@:.nxe=.elf) $@ --need libc.ndl
```

- `-nostdlib` — żadnego startupu/biblioteki od kompilatora (mamy własny `crt0`).
- `-Wl,--emit-relocs` — zachowaj relokacje `R_386_32` (mknx buduje z nich tabelę relokacji).
- `-T user/nx.ld` — link pod bazą `0x800000` (skrypt niżej).
- `--need libc.ndl` — zapisz w nagłówku, że program potrzebuje `libc.ndl`.

### 3.3 Linker script `user/nx.ld`

Pełna treść (programy, baza `0x800000`, nagłówek `.nxheader` jako pierwszy):

```ld
ENTRY(_start)
SECTIONS
{
  . = 0x800000;
  .nxheader : { KEEP(*(.nxheader)) }
  .text     : { *(.text*) }
  .nximports : { *(.nximports) }
  .rodata   : { *(.rodata*) }
  .data     : { *(.data*) }
  /* Sloty IAT z import-library — osobna sekcja PER biblioteka, by nazwa .nxlib.<soname>
     przetrwała linkowanie (mknx czyta ją, by przypisać import do biblioteki). Dodaj
     linię tutaj, wprowadzając nową bibliotekę dzieloną. */
  .nxlib.libc.ndl  : { *(.nxlib.libc.ndl) }
  .nxlib.greet.ndl : { *(.nxlib.greet.ndl) }
  __bss_start = .;
  .bss      : { *(.bss*) *(COMMON) }
  __bss_end = .;
  __nx_image_size = __bss_start - 0x800000;
}
```

> **Uwaga:** jeśli wprowadzasz nową bibliotekę `.ndl`, której program ma używać, dopisz
> tu linię `.nxlib.<soname> : { *(.nxlib.<soname>) }`.

### 3.4 Startup `user/crt0.S`

Kernel wchodzi w `_start` z `esp` wskazującym na obraz argv+envp (SysV i386). `crt0`
publikuje środowisko do libc, ustawia `progname`, woła `main`, potem `exit`:

```nasm
[BITS 32]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __nx_set_environ]
[EXTERN __nx_set_progname]

_start:
    mov ebp, 0
    mov eax, [esp]              ; argc
    lea edx, [esp + 4]         ; argv
    lea ecx, [esp + 8 + eax*4] ; envp  (= esp + 4 + (argc+1)*4)
    push ecx                    ; main arg3: envp
    push edx                    ; main arg2: argv
    push eax                    ; main arg1: argc
    push ecx                    ; __nx_set_environ(envp)
    call __nx_set_environ       ; udostępnij środowisko dla getenv()
    add esp, 4
    mov eax, [edx]              ; argv[0]
    push eax                    ; __nx_set_progname(argv[0]) — dla getprogname()
    call __nx_set_progname
    add esp, 4
    call main                   ; main(argc, argv, envp)
    add esp, 12
    push eax                    ; kod wyjścia main
    call exit                   ; nie wraca (loader longjmp-uje do jądra)
.hang:
    jmp .hang
```

### 3.5 `libc-glue` i magia `nx-dllimport.h`

`user/libc-glue/` to warstwa łącząca picolibc z jądrem NanOS (kod ten ostatecznie żyje
**wewnątrz `libc.ndl`**). Najważniejsze pliki:

| Plik                 | Rola |
|----------------------|------|
| `syscalls.c`         | opakowania syscalli (`int 0x80`): open/read/write/fork/execve/… |
| `cwd.c`              | bieżący katalog, rozwiązywanie ścieżek |
| `sigtramp.S/.c`      | trampolina sygnałów |
| `termios.c`, `ptyutil.c` | terminale / PTY |
| `dirent.c`, `pwd_grp.c`  | katalogi, baza kont |
| `sockets.c`, `resolv*.c` | gniazda + resolver DNS |
| `posixstubs.c`       | stuby (getrlimit/getrusage…) |
| `include/`           | POSIX-owe nagłówki (arpa, net, netinet, sys, dirent.h, netdb.h, poll.h, pty.h…) |

**Dlaczego `nx-dllimport.h`?** picolibc eksponuje `stdin/stdout/stderr` jako *obiekty danych*,
a `errno` jako *zmienną*. Program odwołuje się do nich po adresie — czego biblioteka dzielona
nie może spełnić bez pośrednictwa typu dllimport. Force-include `nx-dllimport.h` przedefiniowuje
każdy z tych symboli na dereferencję slotu IAT (`__imp_<name>`), który loader wypełnia adresem
symbolu wewnątrz `libc.ndl` — dokładnie model `__declspec(dllimport)` z Windows:

```c
extern FILE **__imp_stdin;
extern FILE **__imp_stdout;
extern FILE **__imp_stderr;
extern int   *__imp_errno;
extern char ***__imp_environ;

#define stdin   (*__imp_stdin)
#define stdout  (*__imp_stdout)
#define stderr  (*__imp_stderr)
#define errno   (*__imp_errno)
#define environ (*__imp_environ)
```

Dlatego programy są kompilowane z `DYNHDR`, a glue (które *jest* libc) — bez.

### 3.6 Jak budowane są biblioteki `.ndl`

Reguła `.ndl` różni się tylko skryptem linkera (`user/dll.ld`, baza `0x09000000`) i flagami
`mknx --dll`. Przykład bibliotki demo `greet.ndl`:

```makefile
$(BINFOLDER)greet.ndl: $(BINFOLDER)nxhdr.o $(BINFOLDER)greet.o $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $(BINFOLDER)greet.elf \
	  $(BINFOLDER)nxhdr.o $(BINFOLDER)greet.o
	$(MKNX) $(BINFOLDER)greet.elf $@ --dll --export nx_greet --export nx_greeting
```

`libc.ndl` powstaje analogicznie, ale z `--export-all`, a do tego generowana jest
**import library** `libc.ndl.a` (`mknx … --implib --export-all --soname libc.ndl`), która dla
każdego eksportu wytwarza thunk `name: jmp [__imp_name]` + slot IAT w sekcji `.nxlib.libc.ndl`.
Program linkuje się z `libc.ndl.a`, więc ściąga tylko realnie używane symbole.

### 3.7 `user/init.c` — PID 1 (kontekst)

`init` to pierwszy program userland. Jego `main` w skrócie:

```c
int main(void) {
	run_dhcp();        /* podnieś eth0 przez DHCP (kernelowy static = fallback) */
	start_services();  /* fork+exec inetd + darkhttpd */
	struct passwd* pw = getpwuid(getuid());
	const char* shell = (pw && pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : FALLBACK_SHELL;
	/* … exec powłoki z przejęciem PID 1 … */
}
```

Powłoka domyślna pochodzi z 7. pola `/nanos/config/passwd` — edycja tego pliku zmienia
domyślny shell.

### 3.8 Aplikacja okienkowa (NanWM)

Aplikacja GUI to **ten sam wbudowany program `.nxe`** co wyżej — tyle że zamiast pisać do
konsoli, jest **klientem kompozytora NanWM** (`nwm`). Kompozytor już istnieje; Twoja apka
łączy się z nim po odziedziczonej parze potoków (fd 3/4), tworzy okno, rysuje piksele i
odbiera zdarzenia. Pełną architekturę (kompozytor, protokół, pełne API) opisuje
**`windowing.md`** — tu jest sam przepis „jak napisać apkę".

**Wybierz bibliotekę:**

| Biblioteka | Kiedy | Przykłady |
|------------|-------|-----------|
| **`libnwui.ndl`** (toolkit) | typowa apka: przyciski, listy, pola tekstowe, layout, menu | `nwform`, `nwexp`, `nwset`, `nwabout` |
| **`libnw.ndl`** (surowo) | własny rendering: rysujesz piksele w buforze okna i obsługujesz zdarzenia ręcznie | `nwnote`, `nwterm` |

#### Wariant A — toolkit (`libnwui`, zalecany)

Budujesz drzewo widżetów, podpinasz callbacki, `nwui_run()` robi pętlę zdarzeń. Plik
`user/mygui/mygui.c` (wzorowane na `nwform`):

```c
#include "nwui.h"

static void on_click(nwui_node *self, void *user) {
    nwui_set_text((nwui_node *) user, "Kliknięto!");
}

int main(void) {
    nwui *u = nwui_open("MyGUI", 320, 180);     /* connect + create_window za Ciebie */
    if (!u) return 1;
    nwui_node *out = nwui_label(u, "");
    nwui_set_root(u, nwui_pad(nwui_column(u,
        nwui_label(u, "Witaj w NanWM"),
        nwui_button(u, "Naciśnij", on_click, out),
        out,
        (nwui_node *) 0), 12));
    nwui_run(u);                                 /* pętla aż okno zamknięte */
    return 0;
}
```

#### Wariant B — surowo (`libnw`)

Dostajesz bufor pikseli okna (BGRX, `0x00RRGGBB`), rysujesz prymitywami `nw_gfx`,
`nw_commit()` wypycha uszkodzony prostokąt, a `nw_next_event()` to Twoje
`GetMessage`/`DispatchMessage` (wzorowane na `nwnote`):

```c
#include "libnw.h"
#include "nw_gfx.h"

int main(void) {
    nw_display *d = nw_connect();
    nw_win *win = nw_create_window(d, 360, 220, "note");
    struct nw_surface s; nw_win_surface(win, &s);
    nw_fill_rect(&s, 0, 0, s.w, s.h, 0x00f4f4ec);
    nw_text(&s, 8, 8, "hello", 0x00101014);
    nw_commit(win, 0, 0, s.w, s.h);
    struct nw_event ev;
    while (nw_next_event(d, &ev, -1) == 1) {
        if (ev.type == NW_EV_CLOSE) break;
        /* NW_EV_KEY / NW_EV_POINTER / NW_EV_FOCUS / NW_EV_PASTE / NW_EV_MENU ... */
    }
    return 0;
}
```

#### Build — reguły `Makefile`

Apki GUI **nie** używają generycznej reguły `%.nxe` (która linkuje tylko `libc.ndl`). Mają
**własną regułę linkowania**: dorzucają import-library toolkitu/klienta i deklarują
`--need libnwui.ndl` (lub `--need libnw.ndl`). Rekurencyjny loader sam dociąga resztę
łańcucha (`libnwui → libnw → libc`), jak windowsowa apka linkująca `user32` dostaje `ntdll`.

```makefile
# 1) lista programów + klasyfikacja jako APLIKACJA (bundle /apps + symlink /bin)
USER_PROGS=... mygui
APP_PROGS=...  mygui

# 2) reguła kompilacji dla katalogu apki (każda apka GUI ma własny podkatalog user/<name>/)
$(BINFOLDER)%.o: user/mygui/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@

# 3a) Wariant A (toolkit): linkuj libnwui.ndl.a + libc.ndl.a, --need libnwui.ndl
$(BINFOLDER)mygui.nxe: $(DYN_GLUE) $(BINFOLDER)mygui.o $(BINFOLDER)libnwui.ndl.a \
                       $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl \
                       $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)mygui.elf \
	  $(DYN_GLUE) $(BINFOLDER)mygui.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)mygui.elf $@ --need libnwui.ndl

# 3b) Wariant B (surowo): zamień libnwui -> libnw wszędzie powyżej i --need libnw.ndl
```

> `libnwui.ndl`/`libnw.ndl`/`libc.ndl` jako prerekwizyty pilnują, że biblioteki są zbudowane
> i trafiają do `/nanos/lib`. Same biblioteki budują się regułami z sekcji 3.6.

#### Uruchomienie

```sh
make image && make run
```

W systemie najpierw odpal kompozytor: `nwm`. Startuje pulpit (Terminal + Settings + Files) i
przejmuje ekran. Twoją apkę uruchomisz:

- z paska/skrótu **Super+R** (Run) wpisując nazwę, albo
- programowo: `nwui_spawn(u, "mygui")` / `nw_spawn(d, "mygui")` z innej apki, albo
- żeby startowała razem z pulpitem — dopisz `spawn_client(slot, "/disks/main/apps/mygui/mygui.nxe")`
  w `user/nwm/nwm.c` (jak `nwterm`/`nwset`/`nwexp`).

Apka — jako `APP_PROGS` — ląduje w bundlu `/apps/mygui/mygui.nxe` z symlinkiem `/bin/mygui.nxe`,
więc działa „po nazwie".

#### Inny język (Rust)

ABI `libnwui` jest czystym C (uchwyty opaque, POD, callbacki), więc apkę GUI da się napisać w
dowolnym języku z C FFI. `rustform` to dokładnie ten sam formularz co `nwform`, ale w Rust:
`cargo` buduje `no_std` staticlib pod cel `i686-nanos.json` (`-Z build-std=core,alloc`), a
linkuje się go z `crt0` + `libnwui.ndl.a` + `libc.ndl.a` i `mknx --need libnwui.ndl` jak każdą
apkę (patrz reguła `rustform` w `Makefile`).

---

## 4. Ścieżka B: port zewnętrznej aplikacji (nanos-sdk)

Gdy chcesz uruchomić **istniejący** program (GNU/BSD, autotools/cmake/meson/make) bez
przepisywania go — używasz `nanos-sdk`. Idea: cały port to mały plik **`nxport.toml`** + (czasem)
parę hooków; sterownik `nanos-port` ściąga źródła, konfiguruje pod `i686-nanos`, buduje i
przepuszcza wynik przez `mknx`.

### 4.1 Oficjalny przepis (`nanos-sdk/docs/PORTING.md`)

```
1. Zrób fork repo aplikacji (np. vim-nanos) ze źródłami upstream (lub pobieraj je przez `source`).
2. Dodaj nxport.toml.
3. Zbuduj: `nanos-port .` w kontenerze nanos-sdk (toolchain na PATH, sysroot zsynchronizowany
   z gotowego NanOS przez `sync-sysroot --nanos=/ścieżka`). Powstaje <name>.nxe + <name>.install.
4. W NanOS: dedykowany target (np. `make ping`) buduje port i kopiuje <name>.nxe do bin/;
   `make image` instaluje go.
```

Toolchain to prawdziwy cross-target: `i686-nanos-gcc -dumpmachine` → `i686-nanos`, zdefiniowane
`__nanos__`, `./configure --host=i686-nanos` akceptowane od ręki.

### 4.2 Format `nxport.toml` — wszystkie pola

| Pole              | Typ            | Domyślnie       | Znaczenie |
|-------------------|----------------|-----------------|-----------|
| `name`            | string         | (wymagane)      | nazwa aplikacji → plik `.nxe` |
| `source`          | string         | (wymagane)      | `git:URL[@ref]`, `tar:URL`, lub `dir:ścieżka` |
| `build`           | string         | `"autotools"`   | `autotools` / `cmake` / `meson` / `make` |
| `configure`       | array[string]  | `[]`            | flagi przekazane do configure/cmake/meson/make |
| `cache`           | array[string]  | `[]`            | odpowiedzi autoconf dla cross-compile (dopisywane do `config.cache`) |
| `make_target`     | string         | (opcjonalne)    | konkretny target make (domyślnie: all) |
| `needs`           | array[string]  | `["libc.ndl"]`  | biblioteki `.ndl` → `mknx --need <name>` |
| `binary`          | string         | `<name>`        | ścieżka do zlinkowanego ELF-a względem źródeł |
| `data`            | array[string]  | `[]`            | pliki danych: `["src/ścieżka -> /docelowa/ścieżka"]` |
| `install`         | string         | `/apps/<name>`  | miejsce w obrazie NanOS (lub `sysroot` dla portu-biblioteki) |
| `sysroot_libs`    | array[string]  | (opcjonalne)    | biblioteki do wgrania do sysroot (gdy `install="sysroot"`) |
| `sysroot_headers` | array[string]  | (opcjonalne)    | nagłówki do wgrania do sysroot (gdy `install="sysroot"`) |

Wartość specjalna `install = "sysroot"` → port-biblioteka (instaluje się do sysroot toolchaina,
nie produkuje `.nxe`).

### 4.3 Najmniejszy możliwy port — przykład `hello`

Z `nanos-sdk/examples/hello/`:

`nxport.toml`:
```toml
name = "hello"
source = "dir:src"
build = "make"
install = "/apps/hello"
```

`src/hello.c`:
```c
#include <stdio.h>
int main(void){ printf("hello from i686-nanos\n"); return 0; }
```

`src/Makefile`:
```makefile
hello: hello.c
	$(CC) -O2 -o hello hello.c
```

`$(CC)` rozwiązuje się do `i686-nanos-gcc`, który sam dokleja `crt0.o`, `nxhdr.o`, `nx.ld` i
`--emit-relocs` (przez spec w `toolchain/nanos.h`).

### 4.4 Realne przykłady (autotools)

**`ping` (GNU inetutils)** — `~/Projects/nanos-sdk-work/inetutils-port/nxport.toml`:

```toml
name = "ping"
source = "dir:inetutils-2.5"
build = "autotools"
configure = [
  "--disable-servers", "--disable-clients", "--enable-ping",
  "--disable-rpath", "--without-libidn2", "--without-libcrypt", "--disable-ipv6",
]
cache = [
  "ac_cv_func_select=yes",
  "gl_cv_func_select_supports0=yes",
  "ac_cv_func_getaddrinfo=yes",
  "ac_cv_func_socket=yes",
  "ac_cv_func_connect=yes",
  # … (dziesiątki odpowiedzi sterujących gnulib — patrz oryginalny plik)
]
needs = ["libc.ndl"]
binary = "ping/ping"
install = "/nanos/bin"
```

**`wget` (HTTP-only, bez TLS)** — kluczowe pola:

```toml
name = "wget"
source = "dir:wget-1.21.4"
build = "autotools"
configure = [
  "--without-ssl", "--disable-ipv6", "--disable-nls", "--without-libpsl",
  "--without-libidn", "--without-zlib", "--disable-pcre", "--disable-pcre2",
  "--without-metalink", "--without-cares",
]
cache = [ /* te same wymuszenia gnulib + stdio-ext + link/pathconf */ ]
needs = ["libc.ndl"]
binary = "src/wget"
install = "/nanos/bin"
```

> **Dlaczego tyle wpisów w `cache`?** Przy cross-compile `configure` nie może uruchamiać binarek
> docelowych, więc testy runtime trzeba „odpowiedzieć" z góry. Bazowe odpowiedzi (rozmiary
> typów, endianness) są w `nanos-sdk/port/config.cache`; port dopisuje swoje. Sporo wpisów
> zmusza gnulib do użycia własnych implementacji GNU tam, gdzie picolibc ma wariant BSD.

### 4.5 Przykłady non-autotools i hooki

**`darkhttpd` (zwykły make):**

```toml
name = "darkhttpd"
source = "dir:darkhttpd"
build = "make"
configure = ["darkhttpd", "CFLAGS=-O2 -DNO_IPV6"]
needs = ["libc.ndl"]
binary = "darkhttpd"
install = "/nanos/bin"
```

**`inetd` z hookiem (wiele binarek z jednego buildu).** Manifest buduje `inetd`, a
`hooks/post_build.sh` wyciąga dodatkowe binarki (`telnetd`, `telnet`, `ifconfig`, `traceroute`)
z tego samego drzewa:

```sh
#!/bin/sh
mknx_if() {   # $1 = zbudowany ELF, $2 = nazwa .nxe
  if [ -f "$1" ]; then
    i686-nanos-mknx "$1" "$PORT/$2" --need libc.ndl && echo "== produced $PORT/$2 =="
  fi
}
mknx_if "$STAGE/telnetd/telnetd"    telnetd.nxe
mknx_if "$STAGE/telnet/telnet"      telnet.nxe
mknx_if "$STAGE/ifconfig/ifconfig"  ifconfig.nxe
mknx_if "$STAGE/src/traceroute"     traceroute.nxe
```

**Hooki** (uruchamiane jeśli istnieją w `hooks/`):

| Hook                  | Kiedy |
|-----------------------|-------|
| `pre_configure.sh`    | po pobraniu źródeł, przed configure |
| `post_configure.sh`   | po configure, przed buildem |
| `post_build.sh`       | po buildzie, przed/zamiast mknx |

Środowisko hooka: `$STAGE` (katalog źródeł, np. `/tmp/port-wget`), `$PORT` (katalog portu).

### 4.6 Jak `nanos-port` buduje każdy system

Sterownik (`nanos-sdk/port/nanos-port`, Python 3) dla każdego `build`:

- **autotools:** scala bazowy `config.cache` z `cache` z manifestu, potem
  `./configure --host=i686-nanos --cache-file=<scalony>` + flagi, potem `make -j`.
- **cmake:** `cmake -DCMAKE_TOOLCHAIN_FILE=…/toolchain-nanos.cmake <flagi> ..` w `build-nanos/`, potem `make -j`.
- **meson:** `meson setup --cross-file …/nanos-cross.meson <flagi> build-nanos`, potem `ninja -C`.
- **make:** `make CC=i686-nanos-gcc -j <flagi>`.

Cross-file CMake (`port/toolchain-nanos.cmake`):
```cmake
set(CMAKE_SYSTEM_NAME nanos)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER   i686-nanos-gcc)
set(CMAKE_CXX_COMPILER i686-nanos-g++)
set(CMAKE_AR           i686-nanos-ar)
set(CMAKE_RANLIB       i686-nanos-ranlib)
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

Cross-file Meson (`port/nanos-cross.meson`):
```ini
[binaries]
c = 'i686-nanos-gcc'
cpp = 'i686-nanos-g++'
ar = 'i686-nanos-ar'
strip = 'i686-nanos-strip'
pkg-config = 'i686-nanos-pkg-config'

[host_machine]
system = 'nanos'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
```

**Honest conftest.** `i686-nanos-gcc` jest opakowane skryptem: normalnie linkuje z
`--unresolved-symbols=ignore-all` (by stockowy kod mógł odwołać się do DANYCH z `libc.ndl`,
które `mknx` zamienia na auto-importy). Ale dla programu próbnego autoconf (`conftest`) wrapper
dokłada `--unresolved-symbols=report-all` + stub danych, żeby `AC_CHECK_FUNC` **uczciwie**
wykrywał brakujące funkcje (inaczej każda funkcja „istnieje" i gnulib generuje błędne
`HAVE_<fn>=1`).

### 4.7 Uruchomienie portu z poziomu NanOS

Porty są napędzane **dedykowanymi, powtarzalnymi targetami** w `Makefile` NanOS (nie ogólnym
`make port APP=`). Każdy target:

1. sprawdza, że toolchain SDK istnieje (`$(SDK_TC)/i686-nanos/include`),
2. **odświeża sysroot z TEGO checkoutu** (kopiuje `user/libc-glue/include/.`, `SyscallNr.h`,
   `libc.ndl.a` → `libc.a`, `libc.ndl`),
3. uruchamia `nanos-port` w kontenerze `nanos-sdk-dev`,
4. kopiuje powstałe `.nxe` do `bin/`.

Realny przykład (`make ping`):

```makefile
NANOS_SDK ?= $(HOME)/Projects/nanos-sdk
SDK_WORK  ?= $(HOME)/Projects/nanos-sdk-work
SDK_TC    := $(SDK_WORK)/toolchain
PING_PORT := $(SDK_WORK)/inetutils-port

ping: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(PING_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:…:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(PING_PORT)/ping.nxe" $(BINFOLDER)ping.nxe
```

Istniejące targety: `make ping`, `make wget`, `make inetd`, `make httpd`, `make udhcpc`
(powtarzalne, przez nanos-port); `make grep`, `make vim`, `make bzip2`, `make bash`
(kopiują/budują z własnych drzew); `make externals` (kopiuje hurtem wszystkie już zbudowane).
Po każdym z nich → `make image`.

> **Dodanie nowego portu** sprowadza się do: (a) katalogu w `$(SDK_WORK)/<app>-port/` z
> `nxport.toml` (+ ew. `hooks/`), (b) skopiowania wzorca targetu w `Makefile` z podmianą nazw.

### 4.8 Budowa toolchaina i sysroot (jednorazowo)

- `nanos-sdk/build-toolchain.sh` buduje binutils (2.43) + gcc (14.2.0) dla `i686-nanos` do
  `/opt/i686-nanos` (patche z `toolchain/` uczą je systemu `nanos`; `toolchain/nanos.h` to
  target-config gcc definiujący `STARTFILE_SPEC`, `LINK_SPEC`, `__nanos__`).
- `nanos-sdk/sysroot/sync-sysroot --nanos=/ścieżka/NanOS` wypełnia sysroot: nagłówki picolibc +
  glue + `SyscallNr.h`, `libc.ndl.a`→`libc.a`, `crt0.o`, `nxhdr.o`, `nx.ld`, `libc.ndl`, oraz
  kompiluje `i686-nanos-mknx` z `tools/mknx.c`. Cała maszyneria jest zamrożona w obrazie
  `nanos-sdk-dev`.

---

## 5. Format `.nxe`/`.ndl` w szczegółach

Z `kernel/NxFormat.h` (wspólne dla jądra i `tools/mknx.c`):

```c
#define NX_MAGIC    0x0045584E   /* 'N','X','E',0 little-endian */
#define NX_VERSION  3
#define NX_FLAG_DLL 1u           /* bit 0 flags: moduł to biblioteka dzielona */

typedef struct {                 /* import: rozwiąż nameOff w bibliotece libOff, wpisz do slotAddr */
	unsigned nameOff;            /* adres nazwy importu */
	unsigned slotAddr;           /* adres slotu IAT do załatania */
	unsigned libOff;             /* adres nazwy biblioteki źródłowej, lub 0 = płaskie */
} NxImport;

typedef struct {                 /* export: nameOff wołalny pod addr po załadowaniu */
	unsigned nameOff;
	unsigned addr;
} NxExport;

typedef struct {                 /* relokacja bazowa: słowo 32-bit pod off dostaje deltę ładowania */
	unsigned off;
} NxReloc;

typedef struct {                 /* potrzebna biblioteka: załaduj .ndl o nazwie nameOff przed importami */
	unsigned nameOff;
} NxNeeded;

typedef struct {
	unsigned magic, version, flags;
	unsigned entry;              /* adres wejścia (.nxe) */
	unsigned loadBase;           /* preferowana baza linkowania */
	unsigned imageSize;          /* bajtów zapisanych w pliku */
	unsigned bssStart, bssEnd;   /* zerowane przez loader */
	unsigned importTable, importCount;
	unsigned exportTable, exportCount;
	unsigned relocTable,  relocCount;
	unsigned neededTable, neededCount;
} NxHeader;
```

**`mknx` (CLI)** — `tools/mknx.c`, budowane przez `cc -O2 -Wall -Ikernel -o bin/mknx tools/mknx.c`:

```
mknx <in.elf> <out.nxe|out.ndl> [--dll] [--export NAME]... [--export-all] [--need NAME]...
mknx <in.elf> <out.s>           --implib [--export NAME]... [--export-all] [--soname NAME]
```

Co robi: czyta ELF32 z relokacjami, wyznacza `loadBase`/`bss`, kopiuje sekcje alokowane do
ciągłego obrazu, zbiera relokacje `R_386_32`, buduje tabele import/export/needed, skleja
wszystko z nagłówkiem (adresy w nagłówku są absolutne). W trybie `--implib` generuje katalog z
`.s` per eksport (thunk + slot IAT) → potem `nasm` + `ar` dają `libc.ndl.a`.

---

## 6. Jak kernel ładuje i uruchamia program

- **Baza ładowania programu:** `0x800000` (`STAGE_BASE` w `kernel/Exec.cpp`, zgodne z
  `user/nx.ld`).
- **Biblioteki** ładują się w „paśmie modułów" — `kernel/DynLoader.cpp` przydziela kolejne
  bazy co `arch::mmuModuleStride()`.
- **Per-DLL namespace:** każda załadowana biblioteka ma własną tablicę symboli (kluczowaną
  soname); import wskazuje swoją bibliotekę przez sekcję `.nxlib.<soname>`.
- **Rekurencyjny loader:** ładując `.ndl`, najpierw ładuje wszystkie jej `NEEDED` (post-order),
  więc klient linkujący `libnw.ndl` dostaje automatycznie `libc.ndl` (model dyld/Windows).
- **Relokacja:** delta = `realna_baza − loadBase`; `NxeLoader` aplikuje ją do każdego miejsca z
  `NxReloc[]`.
- **Wiązanie IAT:** każdy slot `NxImport` łatany adresem symbolu z tablicy docelowej biblioteki.
- **Wejście w ring 3:** kernel tworzy `AddressSpace` procesu, mapuje obraz + 512 KiB stosu
  (`archLoadUser`) i wchodzi w ring 3 przez `iret` (`archEnterUser`, selektory `0x1B`/`0x23`).
- **Syscalle i wyjście:** program woła jądro przez `int 0x80`; `exit()` trafia do `procExit`,
  który zwalnia przestrzeń adresową i oddaje sterowanie schedulerowi (model trap-frame z FAZY 4 —
  **nie** setjmp/longjmp).

> Pełny opis formatu i runtime'u (loadery `NxeLoader`/`DynLoader`, mapa pamięci userland,
> przejście w ring 3, fork/execve, sygnały) → **`nxe-ndl.md`**.

---

## 7. Gdzie aplikacja ląduje na dysku

Pełny opis layoutu → `filesystem.md`. W skrócie, build wpisuje pliki przez `debugfs`:

```
/disks/main/
├── nanos/
│   ├── bin/            narzędzia SYSTEMOWE (płasko): nsh, cat, ls, free, ping, wget, grep…
│   └── lib/            biblioteki dzielone: libc.ndl, libnw.ndl, libnwui.ndl…
├── apps/
│   └── <name>/         bundle aplikacji NIEsystemowej:
│       ├── <name>.nxe
│       └── …           własne dane (np. apps/doom/doom1.wad)
└── bin/                LINK FARM: /bin/<name>.nxe -> /apps/<name>/<name>.nxe (symlink)
```

- **`SYS_PROGS`** → `/nanos/bin/<name>.nxe` (narzędzie systemowe).
- **`APP_PROGS`** → `/apps/<name>/<name>.nxe` + symlink `/bin/<name>.nxe` (aplikacja w bundlu).
- **`USER_LIBS_NDL`** → `/nanos/lib/<name>.ndl`.

Powłoka (`user/nsh.c`) rozwiązuje gołą nazwę komendy w kolejności:
`/nanos/bin/<cmd>.nxe` → `/bin/<cmd>.nxe` (link farm) → `/apps/<cmd>/<cmd>.nxe`.

---

## 8. API dostępne dla aplikacji (libc + syscalle)

- **C library:** picolibc (printf/scanf, malloc/free, string.h, math, stdio na plikach…).
- **POSIX (przez glue):** open/read/write/close/lseek, fork/execve/wait, pipe, dup,
  signals (sigaction + trampolina), termios, PTY, dirent (opendir/readdir),
  pwd (getpwuid), gniazda BSD (socket/bind/connect/sendto/recvfrom), resolver
  (getaddrinfo/gethostbyname), clock_gettime (monotoniczny), nanosleep, getrlimit (stub).
- **Numery syscalli:** `kernel/SyscallNr.h` (Linux-style i386, `int 0x80`).
- **GUI:** `libnw.ndl` (klient kompozytora NanWM) + `libnwui.ndl` (toolkit UI) — jak napisać
  apkę okienkową: **sekcja 3.8**; architektura + pełne API: **`windowing.md`**. Przykłady:
  `nwnote`/`nwterm` (surowo, libnw), `nwform`/`nwexp`/`nwset`/`nwabout` (toolkit, libnwui),
  `rustform` (toolkit z Rust).

Czego **nie ma** (na dziś): TLS/OpenSSL (więc `wget` to HTTP-only), IPv6, pełne locale/NLS,
threads. Przy portach to się odzwierciedla we flagach `configure` (`--without-ssl`,
`--disable-ipv6`, `--disable-nls`).

---

## 9. Debugowanie i typowe problemy

**Ścieżka A (wbudowane):**

- *Pułapka case-insensitive macOS:* nie polegaj na `-Ilib` z `<string.h>` — build kopiuje
  źródła na case-sensitive FS kontenera. To już ogarnięte w Makefile; pamiętaj tylko, że
  programy userland używają picolibc (`-isystem`), nie nagłówków jądra.
- *Brak `nx-dllimport.h`:* jeśli program odwołuje się do `stdout`/`errno` i dostajesz
  „undefined reference" przy `mknx` — upewnij się, że plik jest budowany regułą `user/%.c`
  (z `DYNHDR`), a nie jako glue.
- *Nowa biblioteka `.ndl`:* dodaj sekcję `.nxlib.<soname>` w `user/nx.ld`, inaczej importy z
  niej nie zostaną otagowane.

**Ścieżka B (porty):**

- *`configure` zawiesza się / źle wykrywa funkcje:* to cross-compile — uzupełnij `cache` w
  `nxport.toml` (wzoruj się na `inetutils-port`/`wget-port`). Honest-conftest sprawia, że
  brakujące funkcje są zgłaszane uczciwie.
- *Brakujące nagłówki:* dodaj je do `user/libc-glue/include/` w NanOS i przebuduj — targety
  portujące odświeżają sysroot z tego checkoutu przed buildem.
- *gcc 14 promuje warning do błędu:* dorzuć `CFLAGS=-Wno-error=…` w `configure` (build flag,
  nie patch źródeł) — patrz manifest `inetd`.

**Weryfikacja w QEMU (headless):** zbuduj obraz z `grub.cfg timeout=0`, bootuj z
`-display none -monitor unix:/tmp/qmon,…`, zrzuć ekran (`screendump`), skonwertuj `sips` i
obejrzyj PNG. Do diagnozy faultów: `-no-reboot -d int -D log`, grep `v=0d`/`v=08`.

---

## 10. Checklisty

### Nowa aplikacja wbudowana (Ścieżka A)

- [ ] `user/<name>.c` z `int main(...)`.
- [ ] dopisać `<name>` do `USER_PROGS` w `Makefile`.
- [ ] dopisać do `SYS_PROGS` (→ `/nanos/bin`) lub `APP_PROGS` (→ `/apps/<name>` + link farm).
- [ ] reguła `$(BINFOLDER)<name>.nxe: $(USER_GLUE) $(BINFOLDER)<name>.o $(BINFOLDER)libc.ndl.a`
      (+ dodatkowe `.o`, jeśli program ma więcej plików).
- [ ] (jeśli używasz nowej `.ndl`) sekcja `.nxlib.<soname>` w `user/nx.ld`.
- [ ] `make image && make run`.

### Nowa aplikacja okienkowa (NanWM, sekcja 3.8)

- [ ] `user/<name>/<name>.c` z `int main(...)` używające `nwui.h` (toolkit) lub `libnw.h` (surowo).
- [ ] dopisać `<name>` do `USER_PROGS` **i** `APP_PROGS` (apka GUI = bundle `/apps/<name>`).
- [ ] reguła kompilacji `$(BINFOLDER)%.o: user/<name>/%.c` (apka ma własny podkatalog).
- [ ] **własna** reguła linkowania: `libnwui.ndl.a` (lub `libnw.ndl.a`) + `libc.ndl.a`,
      `mknx … --need libnwui.ndl` (lub `--need libnw.ndl`) — nie generyczna `%.nxe`.
- [ ] `make image && make run`, w systemie uruchom `nwm`, potem apkę (Super+R / `spawn`).

### Nowy port (Ścieżka B)

- [ ] katalog `$(SDK_WORK)/<app>-port/` z `nxport.toml`.
- [ ] `source` (`dir:`/`git:`/`tar:`), `build`, `configure`, `binary`, `install`.
- [ ] `cache` z odpowiedziami cross-compile (kopiuj z istniejącego portu, dostosuj).
- [ ] ew. `hooks/{pre_configure,post_configure,post_build}.sh`.
- [ ] target w `Makefile` NanOS wzorowany na `ping`/`wget` (odśwież sysroot → `nanos-port` →
      kopiuj `.nxe` do `bin/`).
- [ ] `make <app> && make image && make run`.

---

## 11. Mapa plików referencyjnych

**NanOS (`~/Projects/NanOS`):**

| Plik | Co zawiera |
|------|------------|
| `Makefile` (sekcja userland ~501–779) | flagi, reguły kompilacji/linkowania, listy programów, instalacja do obrazu |
| `Makefile` (sekcja external ~47–207) | targety portów (`ping`/`wget`/`inetd`/`httpd`/`udhcpc`/`externals`) |
| `user/nx.ld` | linker script programów (baza `0x800000`) |
| `user/dll.ld` | linker script bibliotek (baza `0x09000000`) |
| `user/crt0.S` | startup `_start → main → exit` |
| `user/libc-glue/` | warstwa POSIX/syscall (kod libc.ndl) + nagłówki w `include/` |
| `user/libc-glue/nx-dllimport.h` | przekierowanie symboli-danych (stdout/errno/…) przez IAT |
| `user/libc-glue/compat-decls.h` | deklaracje brakujące w picolibc |
| `user/libnw/{libnw.h,nwproto.h,nw_gfx.h}` | klient NanWM: API okna/zdarzeń, protokół, rasterizer |
| `user/libnwui/nwui.h` | toolkit UI (widżety, layout, menu) — ABI w czystym C |
| `user/nwm/` | kompozytor (`nwm.nxe`) |
| `user/{nwform,nwnote,nwexp,nwset,nwabout,nwterm}/` | przykładowe apki okienkowe |
| `user/rust/rustform/` | przykładowa apka GUI w Rust (toolkit przez C FFI) |
| `docs/windowing.md` | architektura NanWM + pełne API (uzupełnia sekcję 3.8) |
| `kernel/NxFormat.h` | definicja formatu `.nxe`/`.ndl` |
| `kernel/Exec.cpp`, `kernel/NxeLoader.cpp`, `kernel/DynLoader.cpp` | ładowanie/uruchamianie |
| `kernel/SyscallNr.h` | numery syscalli (i386, `int 0x80`) |
| `tools/mknx.c` | konwerter ELF → `.nxe`/`.ndl` |
| `user/free.c`, `user/init.c` | proste przykłady programów |
| `docs/filesystem.md` | layout systemu plików (gdzie co ląduje) |

**nanos-sdk (`~/Projects/nanos-sdk`):**

| Plik | Co zawiera |
|------|------------|
| `README.md` | przegląd SDK |
| `docs/PORTING.md` | oficjalny przepis na port |
| `build-toolchain.sh` | budowa `i686-nanos` binutils+gcc |
| `toolchain/nanos.h` | target-config gcc (`STARTFILE_SPEC`/`LINK_SPEC`/`__nanos__`) |
| `port/nanos-port` | sterownik portów (Python) |
| `port/config.cache` | bazowe odpowiedzi cross-compile |
| `port/toolchain-nanos.cmake` | cross-file CMake |
| `port/nanos-cross.meson` | cross-file Meson |
| `sysroot/sync-sysroot` | wypełnianie sysroot z buildu NanOS |
| `examples/hello/` | minimalny port (wzorzec do kopiowania) |

**Praca/porty (`~/Projects/nanos-sdk-work`):** `inetutils-port`, `wget-port`,
`inetutils-services-port`, `darkhttpd-port`, `busybox-1.36.1`, `grep-3.11`, `vim`, `bzip2-1.0.8`
— realne, działające manifesty do podejrzenia.

---

*Dokument opisuje stan na czerwiec 2026 (gałąź `dockerized-build`). Format `.nxe` = NX_VERSION 3.*
