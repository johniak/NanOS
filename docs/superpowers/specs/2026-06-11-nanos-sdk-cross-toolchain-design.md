# NanOS SDK — natywny cross-toolchain `i686-nanos` + uniwersalny system portów

Data: 2026-06-11
Status: design (do akceptacji)

## Kontekst

Dziś jedyny zewnętrzny port (GNU bash) ma własne, ręczne rusztowanie w forku
`~/Projects/bash-nanos/nanos/`: wrapper `nx-gcc`, `build.sh`, `config.cache`. Działa, ale jest
bash-specyficzne i nie nadaje się do łatwego powielania na kolejne aplikacje (vim, ncurses,
zlib, less…). Warstwa systemowa NanOS jest już wystarczająca do portów realnych narzędzi:
pełny zapis ext2/ext4 + JBD2, syscalle metadanych + rodzina `*at`, credentials/uprawnienia,
zegar (mtime/ctime), termios + PTY + TIOCGWINSZ, mmap, fork/exec, poll, `libc.ndl` (kompletne,
export-all picolibc + glue).

Brakuje **uniwersalnego SDK**, dzięki któremu port nowej aplikacji autotools/CMake/meson to
kwestia małego manifestu, a nie ręcznego dłubania w skryptach.

## Cel i nie-cele

**Cel:** samodzielne repo `nanos-sdk` dostarczające **prawdziwy cross-toolchain o triplecie
`i686-nanos`** (natywny port binutils+gcc, wariant B2) z sysrootem, plus driver `nanos-port` i
format manifestu, tak by `./configure --host=i686-nanos && make` (oraz CMake/meson) budowały
typowe projekty niemal bez łatek, a wynik instalował się w obrazie NanOS jak każdy bundle.

**Nie-cele (na teraz):** libstdc++ (C wystarcza dla vima; C++ później), biblioteki współdzielone
robione przez kompilator/libtool (nasze „shared" to `.ndl` przez `mknx`; linkujemy statycznie do
`libc.ndl.a`), Rust/Go (triplet to umożliwia w przyszłości, ale nie w pierwszym cięciu),
uruchamianie testów `configure` na targecie (niemożliwe w cross — stąd `config.cache`).

## Decyzje (zatwierdzone)

- **Architektura:** B2 — natywny port GCC/binutils z targetem `i686-nanos` (nie wrappery).
- **Lokalizacja:** osobne repo GitHub `nanos-sdk`; każda aplikacja to osobny fork-repo (jak
  `bash-nanos`) z cienkim manifestem.
- **Manifest:** hybryda — deklaratywny `nxport.toml` dla ~90% przypadków + opcjonalne hooki
  shell (`pre_configure`, `post_build`, …) dla wyjątków.

## Architektura

### 1. Port toolchaina (binutils + gcc → `i686-nanos`)

`config.sub` (wspólny w binutils i gcc): dodać `nanos` jako OS, by `i686-nanos` kanonizowało się
do `i686-pc-nanos`.

**binutils** (`--target=i686-nanos`):
- `bfd/config.bfd`: `i[3-7]86-*-nanos*` → `targ_defvec=i386_elf32_vec`.
- `ld/configure.tgt`: → `targ_emul=elf_i386`.
- `gas/configure.tgt`: → format `elf`.
- Produkuje `i686-nanos-{as,ld,ar,ranlib,objcopy,strip,nm,readelf}`.

**gcc** (`--target=i686-nanos --with-newlib --with-sysroot=/opt/i686-nanos`,
`--enable-languages=c`):
- `gcc/config.gcc`: case `i[3-7]86-*-nanos*` → `tm_file="${tm_file} i386/unix.h i386/att.h
  dbxelf.h elfos.h newlib-stdint.h i386/nanos.h"`, `gnu_ld=yes`, `use_gcc_stdint=wrap`,
  `tmake_file` z libgcc.
- **`gcc/config/i386/nanos.h`** (nowy plik — rdzeń portu):
  - `TARGET_OS_CPP_BUILTINS`: `__nanos__`, `__NanOS__`, `__ELF__`.
  - `STARTFILE_SPEC = "crt0.o%s nxhdr.o%s"` (z sysroot `usr/lib`).
  - `ENDFILE_SPEC = ""`.
  - `LINK_SPEC = "%{!T:-T nx.ld} --emit-relocs -z noexecstack"` (relokacje pod `mknx`).
  - `LIB_SPEC = "-lc"`, libgcc dokładany standardowo.
  - model statyczny (brak shared/dynamic-linker spec).
- Buduje `i686-nanos-gcc`, `i686-nanos-g++` (g++ obecny, libstdc++ wyłączone na start) i
  **libgcc** do sysroota.

### 2. Sysroot `/opt/i686-nanos/`

Kolejność bootstrapu (klasyczny cross-newlib):
1. `usr/include/` ← picolibc + `user/libc-glue/include` + potrzebne nagłówki NanOS — NAJPIERW,
   bo libgcc potrzebuje `limits.h`/`stddef.h` itd.
2. binutils → gcc (gcc instaluje `libgcc.a` do sysroota/target-libdir).
3. Z builda NanOS: `usr/lib/{crt0.o, nxhdr.o, libc.a(=libc.ndl.a), nx.ld}`.

`sync-sysroot --nanos=/ścieżka` odświeża artefakty z bieżącego builda NanOS (`libc.ndl` zmienia
się wraz z jądrem). `usr/lib/pkgconfig/` na `.pc` portowanych bibliotek.

### 3. `mknx` jako krok po-linkowy

GCC nie odpali `mknx` w specs (to post-link). `i686-nanos-gcc foo.c -o foo` daje ELF z
relokacjami; `i686-nanos-mknx foo foo.nxe --need libc.ndl` robi `.nxe`. `mknx` wędruje do SDK
(źródło `tools/mknx.c` współdzielone z NanOS, budowane natywnym cc) jako `i686-nanos-mknx`.
Conftesty `configure` tylko linkują ELF — `mknx` ich nie dotyczy.

### 4. Driver `nanos-port` + manifest

`nxport.toml`:
```toml
name = "vim"
source = "git:https://github.com/vim/vim @ v9.1.0000"   # | tar:URL
build  = "autotools"                                    # | cmake | meson | make
configure = ["--without-x", "--disable-gui", "--with-tlib=tinfo"]
cache  = ["ac_cv_func_select=yes", "vim_cv_toupper_broken=no"]  # dopiski do bazowego cache
needs  = ["libc.ndl"]                                   # + inne .ndl (np. tinfo.ndl)
data   = ["runtime/ -> /apps/vim/runtime"]
install = "/apps/vim"                                   # bundle + symlink /bin/vim.nxe
```
Opcjonalne hooki: pliki `hooks/pre_configure.sh`, `hooks/post_build.sh` itd. (uruchamiane jeśli
istnieją). Driver: pobierz → (hook) → configure/cmake/meson z `--host=i686-nanos` + cache →
`make` → `mknx` → `<name>.nxe` → zapis artefaktu + metadanych instalacji.

Pliki wspólne SDK: bazowy `config.cache` (generyczne `ac_cv_sizeof_*`, „cross", typowe funkcje),
`toolchain-nanos.cmake`, `nanos-cross.meson`.

### 5. Docker

`nanos-sdk/Dockerfile`: buduje binutils+gcc `i686-nanos` ze źródeł (jak cross NanOS, ~20-40 min,
warstwa cache'owana), + picolibc, + narzędzia portu (autoconf, cmake, meson, pkg-config).

### 6. Integracja z NanOS

- `make sysroot` — eksport `crt0.o/nxhdr.o/libc.ndl.a/nx.ld` do miejsca, z którego SDK je bierze
  (lub `nanos-sdk sync-sysroot` czyta katalog NanOS).
- `make port APP=vim` — woła `nanos-port` w kontenerze `nanos-sdk` (oba repa bind-mount), kopiuje
  `<name>.nxe` do `bin/`.
- `_image` instaluje `.nxe` jako bundle `/apps/<name>` + symlink `/bin/<name>.nxe` + pliki `data`.
- `bash-nanos` przepinamy na `nxport.toml` (dowód uogólnienia), potem `vim-nanos`.

## Układ repo `nanos-sdk`

```
toolchain/            patche: config.sub, binutils (bfd/ld/gas), gcc/config.gcc, gcc/config/i386/nanos.h
build-toolchain.sh    pobiera + patchuje + buduje binutils,gcc -> /opt/i686-nanos
Dockerfile            obraz nanos-sdk (toolchain + picolibc + autoconf/cmake/meson)
sysroot/              skrypty: init-headers, sync-sysroot
bin/                  i686-nanos-mknx (+ ew. cienkie aliasy)
port/nanos-port       driver portu
port/config.cache     bazowy cache cross
port/toolchain-nanos.cmake, port/nanos-cross.meson
docs/                 README, "jak sportować aplikację", przykład nxport.toml
```

## Fazy realizacji (każda = działający, weryfikowalny krok)

1. **Repo + szkielet** `nanos-sdk` (struktura, README, licencja).
2. **Port binutils** `i686-nanos` (config.sub/bfd/ld/gas) → `i686-nanos-as/ld/...` budują i linkują
   testowy `.o`.
3. **Port gcc** + `nanos.h` + libgcc → `i686-nanos-gcc hello.c -o hello` daje ELF; `mknx` →
   `hello.nxe`; **uruchamia się pod NanOS** (QEMU) i drukuje. Bramka: „hello world" działa.
4. **Sysroot + sync** + bazowy `config.cache` + pliki CMake/meson. Bramka: trywialny projekt
   autotools (`--host=i686-nanos`) przechodzi configure+make+mknx.
5. **Driver `nanos-port` + manifest** (toml + hooki). Integracja `make port` w NanOS.
6. **Migracja bash** na `nxport.toml` (regresja: bash nadal działa pod NanOS).
7. **Port vim** (`vim-nanos` + `nxport.toml`): build, instalacja, uruchomienie w QEMU
   (edycja+zapis pliku na `/disks/main`, dzięki write-supportowi ext).
8. **Docs**: przewodnik „portowanie aplikacji w 10 minut".

## Ryzyka

- Iteracyjne dobicie `nanos.h`/specs aż `hello.nxe` ruszy (libgcc, startfile, link spec) — kilka
  rund po długim buildzie gcc. Mitygacja: najpierw mały `crt0`+`-nostdlib` smoke-test, potem
  pełne specs.
- Łatki binutils/gcc do utrzymania przy zmianie wersji — ale małe, lokalne, w `toolchain/`.
- Sprzężenie sysroot↔build NanOS (libc.ndl ewoluuje) — rozwiązane przez `sync-sysroot`.
- vim może odsłonić brakujące syscalle (`select`, `gettimeofday`, `sigaction`) — domkniemy w
  jądrze NanOS w fazie 7 (poza-SDK, ale w zakresie „port vima").

## Weryfikacja

- Faza 3: `hello.nxe` z `i686-nanos-gcc` bootuje i drukuje w QEMU headless (rytuał screendump).
- Faza 4: testowy autotools-projekt buduje się i uruchamia.
- Faza 6: bash przez `nxport.toml` = identyczny `bash.nxe`, uruchamia się jak dziś.
- Faza 7: vim w QEMU otwiera/edytuje/zapisuje plik na `/disks/main`; po sesji `e2fsck -fn` czysty.
