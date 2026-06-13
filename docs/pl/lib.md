# Biblioteka freestanding NanOS (`lib/` + `include/`)

Jądro budowane jest z `-ffreestanding -nostdlib -nostdinc++` — nie ma libc ani STL. `lib/` (plus
`include/string.h`) to mała własna warstwa narzędziowa wypełniająca lukę: funkcje string/memory dla
środowiska freestanding, dwie klasy kontenerów (`String`, `List`) oraz glue C++ ABI, dzięki któremu
kompilator w ogóle może emitować C++. (Userland dostaje *prawdziwą* bibliotekę C przez `libc.ndl` —
@docs/nxe-ndl.md, @docs/writing-apps.md.)

---

## 1. Freestanding string/memory (`include/string.h` + `lib/string_funcs.cpp`)

Freestanding `<string.h>` deklaruje dokładnie to, czego używa jądro — `memcpy`, `memmove`, `memset`,
`strlen`, `strcmp`, `strstr` — zaimplementowane w `lib/string_funcs.cpp`. Ścieżka include jądra
(`KINCLUDES`) ma `-Iinclude`, więc `#include <string.h>` rozwiązuje się do **tego** nagłówka; build
testów hosta (`HINCLUDES`) pomija `-Iinclude`, więc to samo `<string.h>` rozwiązuje się do **libc** i
testy wykorzystują funkcje platformy (CLAUDE.md, Testing).

> Pułapka case-insensitive macOS: z `-Ilib` na ścieżce `#include <string.h>` rozwiązałby się do
> `lib/String.h` (klasy C++) → nieskończona rekurencja include. Dlatego jądro kompiluje się z kopii
> na case-sensitive filesystemie kontenera (CLAUDE.md).

---

## 2. `String` (`lib/String.h`)

Mały string o **semantyce wartości**. Każdy `String` jest właścicielem swojego bufora `char*`: copy
ctor i `operator=` robią głęboką kopię, a destruktor zwalnia. To ma znaczenie, bo `free` sterty
jądra jest teraz prawdziwy (@docs/memory.md §5), a `String` przekazuje się przez wartość w wielu
miejscach wywołań — współdzielone bufory dawałyby double-free / use-after-free.

- konstruktory z `const char*` i z `int` (przez `itoa`);
- `append` (używa `realloc`, który wchłania stary bufor — bez osobnego `free`, który był kiedyś
  double-free); `operator+` dla `String`/`char*`/`int`;
- `indexOf`/`startsWith` (nad `strstr`), `compareTo` (`strcmp`), `getLenght`, `operator[]`,
  `operator char*` (degradacja do surowego bufora).
- `split`/`substring` zostały **usunięte** — zwracały `List<String>`/`String` przez wartość przez
  kontener, który nie może bezpiecznie trzymać typów wartościowych; jedyny użytkownik (wyszukiwanie
  ścieżki ext) parsuje teraz komponenty w miejscu (@docs/filesystem.md).

`String` to waluta ścieżek/stringów w VFS i sterowniku ext.

---

## 3. `List<T>` (`lib/List.h`)

Szablonowa **tablica dynamiczna** (rośnie o `capacityInc`, domyślnie 10): `add`/`insert`/`removeAt`/
`operator[]`/`getCount`. Używa **konsekwentnie rodziny `malloc`/`realloc`/`free`** — *nie* `new[]` —
bo `new[]` dla nietrywialnego `T` dodaje cookie tablicy, którego `realloc`/`free` nie rozumieją
(pierwotny mismatch `new[]`+`free` oraz double-free przy wzroście były błędami naprawionymi tutaj).
`increaseCapacity` polega na `realloc`, że przeniesie/zwolni stary blok.

> Zastrzeżenie: `List<T>` kopiuje elementy przez `memcpy` (bez copy ctor elementu), więc dobrze trzyma
> typy POD / trywialnie kopiowalne, ale nie takie, których kopia wymaga prawdziwego konstruktora —
> co jest właśnie powodem usunięcia `String::split` zwracającego `List<String>`.

---

## 4. Glue C++ ABI (`lib/icxxabi.*`)

Stuby Itanium C++ ABI, żeby freestanding C++ się zlinkował: `__cxa_atexit` / `__cxa_finalize` (+
`__dso_handle`) nad 128-elementową tablicą atexit. Kompilator emituje wywołania `__cxa_atexit` do
rejestracji destruktorów obiektów o statycznym czasie życia; te symbole muszą istnieć, żeby jądro się
zlinkowało, mimo że **globalne konstruktory nie są uruchamiane** (loader skacze prosto do `kmain`,
bez przejścia `.ctors` — CLAUDE.md). W praktyce tablica jest kontraktem link-time, nie runtime'ową
ścieżką teardownu (jądro nigdy nie „wychodzi").

---

## 5. Backing alokacji

`String`/`List` i cała reszta alokują przez `mm/memory_manager.*` → kernelowy `Heap`
(@docs/memory.md §5). `memory_manager` deklaruje `malloc`/`free` z **linkage C++** (nie
`extern "C"`); testy hosta dostarczają pasujące shimy przekierowujące do builtinów libc (CLAUDE.md,
Testing).

**Kluczowe pliki:** `include/string.h`, `lib/{string_funcs.cpp, String.h, String.cpp, List.h,
List.cpp, icxxabi.h, icxxabi.cpp}`, `mm/memory_manager.*`.
