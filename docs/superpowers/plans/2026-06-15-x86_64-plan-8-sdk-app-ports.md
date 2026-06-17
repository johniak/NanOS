# x86_64 Plan 8 — SDK `x86_64-nanos` + przebudowa portów aplikacji

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Postawić cross-toolchain `x86_64-nanos` (mirror istniejącego `i686-nanos`) w repo
`~/Projects/nanos-sdk` i przebudować ~12 zewnętrznych portów aplikacji przeciw 64-bitowemu
sysrootowi, tak by obraz dysku NanOS x86_64 bootował i odpalał `bash`/`nsh` plus reprezentatywny
zestaw narzędzi (`ls`/`cat`/`grep`/`vim`, `ping`, `openssl`, serwer). To **najdłuższy, najbardziej
równoległy i najsłabiej sprzężony z kernelem** strumień całej migracji — startuje dopiero gdy
kernel x86_64 bootuje i odpala `init.nxe` (kamień 6).

**Architecture:** Migracja-zastąpienie (spec §0.1): po cut-over istnieje **tylko** triplet
`x86_64-nanos`; `i686-nanos` znika. Dźwignią całej przebudowy userlandu jest **4-artefaktowy
kontrakt** (spec §5.3b): gdy `<triple>/include` (nagłówki LP64 z `user/libc-glue/include` +
x86_64 `SyscallNr.h`) oraz `<triple>/lib/{libc.a,libc.ndl}` staną się 64-bitowe, **wszystkie
porty przebudują się względem nich** zwykłą rekompilacją. Trzy pod-strumienie sekwencyjne:

- **8a — in-tree userland + `libc.ndl` 64-bit** (repo NanOS): *dostarczony przez Plan 6.* Plan 8
  zaczyna od **potwierdzenia/wyprodukowania** 4 artefaktów kontraktu jako 64-bit.
- **8b — toolchain `x86_64-nanos`** (repo `~/Projects/nanos-sdk`): odwzorować reguły
  `i[3-7]86-*-nanos*` na `x86_64-*-nanos*` w `config.sub` (już OS-agnostyczny — bez zmian),
  `bfd/config.bfd`, `ld/configure.tgt`, `gas/configure.tgt`, `gcc/config.gcc` (baza `i386/x86-64*`),
  `libgcc/config.host`, plus 64-bitowy wariant `gcc/config/i386/nanos.h`; przebudować binutils+gcc
  i picolibc dla `x86_64`.
- **8c — rebuild portów** (zewnętrzne repo `<app>-port/` + `~/Projects/nanos-sdk-work`),
  wewnętrznie równolegle, w kolejności ryzyka: bash → grep/vim/bzip2 → inetutils
  (ping/wget/telnetd/inetd/ifconfig/traceroute) → OpenSSL/Dropbear → NetSurf/Doom.

**Tech Stack:** binutils 2.43 + gcc 14.2.0 (`x86_64-nanos`), picolibc 1.8.6 (x86_64), kontenery
`nanos-build` (kernel/userland) + `nanos-sdk-dev` (sterownik portów `nanos-port`), QEMU
(`qemu-system-x86_64`), headless-screendump z CLAUDE.md.

**Reference (spec `2026-06-15-x86_64-migration-analysis.md`):** §5.3 (mechanika patcha + kontrakt
4 artefaktów + lista portów), §5.4 #1 (model kodu/relokacje — **jedyna nie-mechaniczna decyzja**),
#2 (`%fs.base` — załatwione kernelowo w Planie 6), #3 (SSE w userlandzie), #4 (OpenSSL retarget),
#5 (Doom/NetSurf), #6/#7 (reszta + patch są bounded), §7 kamień 8.

> **Spójność cross-plan (decyzja #1).** Bazy ładowania user trzymane w **dolnych 2 GiB**, model
> **small (non-PIC)**, relokacje **`R_X86_64_64`** (+ `R_X86_64_32S`). Ta sama decyzja jest
> zaszyta w `tools/mknx.c` + `user/nx.ld` (Plan 6) i w `LINK_SPEC` z `nanos.h` (`-T nx.ld
> --emit-relocs`, ten plan). **Wszystkie trzy muszą się zgadzać** — Plan 8 NIE zmienia modelu,
> tylko go odwzorowuje w toolchainie. Jeśli `mknx`/`nx.ld` z Planu 6 użyły innego modelu, to jest
> blokada — patrz Task 1.

> **Czego NIE da się zweryfikować z tego repo.** SDK i porty żyją w osobnych repozytoriach
> (`~/Projects/nanos-sdk`, `~/Projects/nanos-sdk-work/<app>-port`, `~/Projects/bash-nanos`,
> `~/Projects/netsurf-nanos`). Kroki oznaczone **[SDK]** / **[PORT:<app>]** wykonuje się i
> commituje **w tamtych repo** — z poziomu repo NanOS można je tylko wywołać przez cele Makefile
> (`make bash`, `make openssl`, …), które montują tamte drzewa do kontenera. Realna weryfikacja
> wyjścia (**[NANOS]**) = skopiowanie gotowych `.nxe` do `bin/`, `make image`, headless-QEMU boot.

---

## File Structure

| Plik / cel | Repo | Odpowiedzialność | Akcja |
|---|---|---|---|
| `toolchain/patch.sh` | nanos-sdk | dopisać reguły `x86_64-*-nanos*` (bfd/ld/gas/gcc/libgcc) + wybór wariantu `nanos.h` wg `$TARGET` | Modify |
| `toolchain/nanos.h` | nanos-sdk | komentarz reloc (R_386_32 → R_X86_64_64); reszta arch-neutralna | Modify |
| `build-toolchain.sh` | nanos-sdk | `TARGET` parametryzowalny (`x86_64-nanos`), `PREFIX=/opt/x86_64-nanos` | Modify |
| `picolibc-x86_64-elf.txt` | nanos-sdk | meson cross-file picolibc dla x86_64 | Create |
| `Dockerfile` | nanos-sdk | druga warstwa: toolchain + picolibc x86_64; sysroot `/opt/x86_64-nanos` | Modify |
| `Makefile` (cele `ping`/`wget`/`openssl`/… ) | NanOS | ścieżka kontraktu `i686-nanos` → `x86_64-nanos` w cp-do-sysrootu | Modify |
| `openssl-port/hooks/pre_configure.sh` | openssl-port | `CC=x86_64-nanos-gcc`, target Configure 64-bit | Modify |
| `<app>-port/nxport.toml`, `nanos/build.sh` | per-port | przełączenie CC/triplet na `x86_64-nanos`; rebuild | Modify |
| `tests/` (smoke `hello.nxe`) | nanos-sdk | minimalny program weryfikujący toolchain end-to-end | Create |

---

## Task 1: [NANOS] 8a — potwierdzić 4 artefakty kontraktu jako 64-bit

Plan 8 **nie buduje** userlandu — to robi Plan 6 (`make build` toolchainem `x86_64-elf`
produkuje `user/` → `bin/libc.ndl`). Tu tylko twardo potwierdzamy, że artefakty są LP64 i że
decyzja #1 (model kodu/relokacje) jest spójna z `mknx`/`nx.ld`.

- [ ] **Step 1: Zbudować userland 64-bit (jeśli świeży checkout)**

  **[NANOS]** Run: `make build` (kontener `nanos-build`, `ARCH=x86_64` powinno być domyślne po
  cut-over Planu 6; w okresie przejściowym `make ARCH=x86_64 build`).
  Expected: powstają `bin/libc.ndl`, `bin/libc.ndl.a`, `bin/crt0.o`, `bin/nxhdr.o`.

- [ ] **Step 2: Potwierdzić, że `libc.ndl` i jej import-lib są ELF64/x86-64**

  ```bash
  docker run --rm -v "$(pwd)":/src -w /src nanos-build sh -c '
    echo "== libc.ndl =="; x86_64-elf-readelf -h bin/libc.ndl | grep -E "Class|Machine"
    echo "== libc.a (ar) =="; x86_64-elf-ar t bin/libc.ndl.a | head -3
    echo "== crt0.o =="; x86_64-elf-readelf -h bin/crt0.o | grep -E "Class|Machine"'
  ```
  Expected: `Class: ELF64` + `Machine: Advanced Micro Devices X86-64` dla `libc.ndl` i `crt0.o`.

- [ ] **Step 3: Potwierdzić numery syscalli x86_64 w `kernel/SyscallNr.h`**

  **[NANOS]** Run: `grep -E 'SYS_(read|write|open|exit|mmap)\b' kernel/SyscallNr.h`
  Expected (numery **x86_64**, nie i386): `read=0 write=1 open=2 mmap=9 … exit=60`. Jeśli wciąż
  widać i386 (`write=4`, `exit=1`) — Plan 6 niedokończony; **STOP**, dokończyć kamień 6.

- [ ] **Step 4: Potwierdzić spójność decyzji #1 (relokacje) między `mknx` a `nx.ld`**

  ```bash
  grep -nE 'R_X86_64_64|R_X86_64_32S|ELF64' tools/mknx.c | head
  grep -nE '0x[0-9a-fA-F]+|\. =' user/nx.ld | head
  ```
  Expected: `mknx` przetwarza `R_X86_64_64`/`R_X86_64_32S` i makra `ELF64_*`; `nx.ld` ustawia bazę
  w dolnych 2 GiB (np. `0x800000`). To **kontrakt**, który `nanos.h` (Task 3) musi odzwierciedlić
  (`-T nx.ld --emit-relocs`). Rozjazd tutaj = błąd ładowania `.nxe` we wszystkich portach.

- [ ] **Step 5: Potwierdzić, że nagłówki glue są arch-neutralne / LP64**

  **[NANOS]** Run: `grep -rnE 'long|__WORDSIZE|sizeof\(void' user/libc-glue/include | grep -i '32' | head`
  Expected: brak twardo zakodowanego `32` zakładającego ILP32 (np. `#define __WORDSIZE 32`).
  Nagłówki są wspólne dla obu arch; LP64 wynika z toolchaina. Notatka, nie commit.

- [ ] **Step 6: Commit (jeśli były drobne poprawki w `user/`/`SyscallNr.h`)**

  **[NANOS]**
  ```bash
  git add user kernel/SyscallNr.h
  git commit -m "userland: confirm 64-bit libc.ndl + x86_64 syscall numbers (contract artifacts)"
  ```

---

## Task 2: [SDK] 8b — `patch.sh`: dopisać reguły `x86_64-*-nanos*`

**Files (repo `~/Projects/nanos-sdk`):** Modify `toolchain/patch.sh`

Wszystkie kroki w repo `~/Projects/nanos-sdk`. Odwzorowujemy istniejące reguły i686 (spec §5.3
pkt 2, §5.4 #7 — robota bounded, czysty mirror).

- [ ] **Step 1: `config.sub` — bez zmian (już OS-agnostyczny)**

  Patch i686 dodaje `nanos*` jako system, niezależnie od CPU — działa dla `x86_64-nanos` bez
  zmian. Nic nie dotykamy; istniejący blok `config.sub` zostaje.

- [ ] **Step 2: Dopisać reguły bfd/ld/gas dla x86_64 (po blokach i686)**

  W `toolchain/patch.sh`, po linii z `gas/configure.tgt` (obecnie `patch.sh:20`), dodaj:

  ```sh
  echo "== patching bfd/ld/gas: x86_64-nanos reuses x86-64 ELF =="
  sed -i 's@x86_64-\*-elf\* @x86_64-*-nanos* | x86_64-*-elf* @' "$BU/bfd/config.bfd"
  sed -i 's@x86_64-\*-elf\* @x86_64-*-nanos* | x86_64-*-elf* @' "$BU/ld/configure.tgt"
  sed -i '/^  x86_64-\*-elf\*)/i\  x86_64-*-nanos*)			fmt=elf ;;' "$BU/gas/configure.tgt"
  ```

  > Uwaga: w binutils 2.43 wzorce w `bfd/config.bfd` / `ld/configure.tgt` to np.
  > `x86_64-*-elf* | x86_64-*-rtems* | ...`. Powyższy `sed` wstrzykuje `x86_64-*-nanos*` przed
  > `x86_64-*-elf*`. Jeśli dokładny tekst się różni, dopasuj wzorzec do sąsiadującego wpisu
  > `x86_64-*-elf*` (weryfikacja w Step 5).

- [ ] **Step 3: Dopisać tuple `x86_64-*-nanos*` w `gcc/config.gcc` (baza x86-64)**

  Po istniejącym bloku awk dla i686 (obecnie `patch.sh:23-33`), dodaj **drugi** blok awk
  zakotwiczony na case `x86_64-*-elf*)` (a NIE i686), z bazowymi nagłówkami x86-64:

  ```sh
  if ! grep -q 'x86_64-\*-nanos\*' "$GCC/gcc/config.gcc"; then
      awk '
        /^x86_64-\*-elf\*\)$/ {
          print "x86_64-*-nanos*)";
          print "\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64elf.h i386/nanos.h\"";
          print "\t;;";
        }
        {print}
      ' "$GCC/gcc/config.gcc" > "$GCC/gcc/config.gcc.new"
      mv "$GCC/gcc/config.gcc.new" "$GCC/gcc/config.gcc"
  fi
  ```

  > `i386/x86-64elf.h` ustawia 64-bitowe ABI (LP64, SSE2 baseline); `i386/nanos.h` nakłada
  > overrides NanOS (STARTFILE/LINK/LIB SPEC). Mirror dokładnie tego, co robi sąsiedni wpis
  > `x86_64-*-elf*` w danej wersji gcc — dorzuć tylko `i386/nanos.h` na końcu `tm_file`.

- [ ] **Step 4: Dopisać tuple `x86_64-*-nanos*` w `libgcc/config.host`**

  Po bloku i686 (obecnie `patch.sh:37-47`), dodaj blok awk na case `x86_64-*-elf*)`:

  ```sh
  if ! grep -q 'x86_64-\*-nanos\*' "$GCC/libgcc/config.host"; then
      awk '
        /^x86_64-\*-elf\*\)$/ {
          print "x86_64-*-nanos*)";
          print "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"";
          print "\t;;";
        }
        {print}
      ' "$GCC/libgcc/config.host" > "$GCC/libgcc/config.host.new"
      mv "$GCC/libgcc/config.host.new" "$GCC/libgcc/config.host"
  fi
  ```

- [ ] **Step 5: Rozszerzyć blok `== verify ==` o asercje x86_64**

  Na końcu `patch.sh` dopisz:
  ```sh
  grep -q 'x86_64-\*-nanos\*' "$BU/bfd/config.bfd"    && echo "  bfd x86_64: OK"
  grep -q 'x86_64-\*-nanos\*' "$BU/ld/configure.tgt"  && echo "  ld  x86_64: OK"
  grep -q 'x86_64-\*-nanos\*' "$BU/gas/configure.tgt" && echo "  gas x86_64: OK"
  grep -q 'x86_64-\*-nanos\*' "$GCC/gcc/config.gcc"   && echo "  gcc/config.gcc x86_64: OK"
  grep -q 'x86_64-\*-nanos\*' "$GCC/libgcc/config.host" && echo "  libgcc x86_64: OK"
  ```

- [ ] **Step 6: Dry-run patcha na świeżo rozpakowanych źródłach**

  **[SDK]** Run (w kontenerze z deps, np. `nanos-build`):
  ```bash
  PHASE=patch sh build-toolchain.sh   # patrz Task 4 — TARGET=x86_64-nanos
  ```
  Expected: wszystkie linie `... x86_64: OK`. Brak `sed: ... no match` (jeśli jest — wzorzec nie
  pasuje do wersji binutils/gcc, popraw w Step 2/3).

- [ ] **Step 7: Commit (w repo nanos-sdk)**

  **[SDK]**
  ```bash
  git -C ~/Projects/nanos-sdk add toolchain/patch.sh
  git -C ~/Projects/nanos-sdk commit -m "toolchain: teach binutils+gcc the x86_64-nanos target (mirror i686)"
  ```

---

## Task 3: [SDK] 8b — wariant 64-bit `nanos.h`

**Files (nanos-sdk):** Modify `toolchain/nanos.h`

`nanos.h` jest **arch-neutralny** w częściach funkcjonalnych: `STARTFILE_SPEC="crt0.o nxhdr.o"`,
`LINK_SPEC="-T nx.ld --emit-relocs …"`, `LIB_SPEC="-lc"` działają identycznie dla x86_64 (relokacje
emituje gcc + `--emit-relocs`, a interpretuje `mknx` — Plan 6). Jedyna zmiana to **komentarz
relokacyjny** (decyzja #1) — żeby nie wprowadzał w błąd.

- [ ] **Step 1: Zaktualizować komentarz przy `LINK_SPEC`**

  W `toolchain/nanos.h` zamień fragment komentarza (obecnie `nanos.h:30`):
  ```c
  /* Link at the NanOS base via nx.ld (overridable with -T) and keep R_386_32 relocations for mknx.
  ```
  na (arch-aware, R_X86_64_64 dla x86_64):
  ```c
  /* Link at the NanOS base via nx.ld (overridable with -T) and keep absolute relocations for mknx:
   * R_386_32 on i686, R_X86_64_64 (+ R_X86_64_32S) on x86_64 — consistent with tools/mknx.c and the
   * low-2GiB small-model load base (decision #1). gcc emits them; --emit-relocs preserves them.
  ```

- [ ] **Step 2: Potwierdzić, że SPEC-i są arch-neutralne (bez zmian funkcjonalnych)**

  **[SDK]** Run: `grep -nE 'STARTFILE_SPEC|LINK_SPEC|LIB_SPEC|TARGET_OS_CPP' toolchain/nanos.h`
  Expected: `crt0.o%s nxhdr.o%s`, `-T nx.ld --emit-relocs …`, `-lc`, `__nanos__`/`_GNU_SOURCE` —
  identyczne dla obu arch. **Nie** dodajemy tu nic 32/64-specyficznego (krytyczne: SSE/LP64 idą
  z bazowego `i386/x86-64elf.h`, nie stąd).

- [ ] **Step 3: Commit (nanos-sdk)**

  **[SDK]**
  ```bash
  git -C ~/Projects/nanos-sdk add toolchain/nanos.h
  git -C ~/Projects/nanos-sdk commit -m "toolchain: nanos.h reloc comment is arch-aware (R_X86_64_64 on x86_64)"
  ```

---

## Task 4: [SDK] 8b — `build-toolchain.sh` buduje `x86_64-nanos`

**Files (nanos-sdk):** Modify `build-toolchain.sh`

- [ ] **Step 1: Sparametryzować TARGET/PREFIX (zachowując domyślny i686 w okresie przejściowym)**

  W `build-toolchain.sh` zamień (`build-toolchain.sh:8-9`):
  ```sh
  TARGET=i686-nanos
  PREFIX="${PREFIX:-/opt/i686-nanos}"
  ```
  na:
  ```sh
  TARGET="${TARGET:-x86_64-nanos}"          # cut-over default; i686-nanos retired (spec §0.1)
  PREFIX="${PREFIX:-/opt/$TARGET}"
  ```

  > To czyni skrypt jednym wejściem dla obu tripletów (`TARGET=i686-nanos sh build-toolchain.sh`
  > zbuduje stary, jeśli kiedyś trzeba). Po cut-over domyślny = `x86_64-nanos`.

- [ ] **Step 2: Zbudować binutils+gcc dla x86_64-nanos**

  **[SDK]** Run (w kontenerze z deps; ~20–40 min, amd64 emulowany na Apple Silicon):
  ```bash
  docker run --rm -v ~/Projects/nanos-sdk:/sdk -v ~/Projects/nanos-sdk-work:/work \
    -w /sdk nanos-build sh -c 'WORK=/work TARGET=x86_64-nanos sh build-toolchain.sh all'
  ```
  Expected (z końcówki skryptu): `x86_64-nanos-gcc --version` + `-dumpmachine` → `x86_64-nanos`.

- [ ] **Step 3: Smoke — sam toolchain kompiluje i linkuje freestanding obiekt**

  **[SDK]**
  ```bash
  docker run --rm -v ~/Projects/nanos-sdk-work:/work nanos-build sh -c '
    export PATH=/opt/x86_64-nanos/bin:$PATH
    echo "int main(){return 0;}" > /tmp/t.c
    x86_64-nanos-gcc -c /tmp/t.c -o /tmp/t.o && x86_64-nanos-readelf -h /tmp/t.o | grep Machine'
  ```
  Expected: `Machine: Advanced Micro Devices X86-64`.

- [ ] **Step 4: Commit (nanos-sdk)**

  **[SDK]**
  ```bash
  git -C ~/Projects/nanos-sdk add build-toolchain.sh
  git -C ~/Projects/nanos-sdk commit -m "build: default the cross toolchain target to x86_64-nanos"
  ```

---

## Task 5: [SDK] 8b — picolibc dla x86_64

**Files (nanos-sdk):** Create `picolibc-x86_64-elf.txt`; Modify `Dockerfile`

picolibc dostarcza nagłówki + libm/libc-math do sysrootu (syscall-glue daje `libc.ndl`).
Budowany przez Dockerfile SDK (obecnie i686-elf — `Dockerfile:16-25`). Mirror dla x86_64.

- [ ] **Step 1: Utworzyć cross-file picolibc x86_64**

  **[SDK]** Utwórz `~/Projects/nanos-sdk/picolibc-x86_64-elf.txt` (mirror `picolibc-i686-elf.txt`):
  ```ini
  # Meson cross file: build picolibc with the NanOS x86_64-elf cross-gcc.
  [binaries]
  c = 'x86_64-elf-gcc'
  ar = 'x86_64-elf-ar'
  as = 'x86_64-elf-as'
  strip = 'x86_64-elf-strip'

  [host_machine]
  system = 'none'
  cpu_family = 'x86_64'
  cpu = 'x86_64'
  endian = 'little'

  [properties]
  skip_sanity_check = true
  ```

  > Używa `x86_64-elf-*` (toolchain z `docker/Dockerfile` NanOS, dostępny w bazowym obrazie SDK —
  > tak jak i686-elf dziś). picolibc 1.8.6 oficjalnie wspiera x86_64 (spec §5.4 #7).

- [ ] **Step 2: Dodać warstwę picolibc x86_64 w Dockerfile SDK**

  W `~/Projects/nanos-sdk/Dockerfile`, po istniejącym bloku picolibc i686-elf (`Dockerfile:16-25`),
  dodaj bliźniaczy blok:
  ```dockerfile
  # picolibc for x86_64 (the long-mode sysroot). Mirror of the i686-elf block above.
  COPY picolibc-x86_64-elf.txt /tmp/pico64.txt
  RUN git clone --depth 1 --branch "${PICOLIBC_VERSION}" https://github.com/picolibc/picolibc /tmp/picolibc \
      && cd /tmp/picolibc && meson setup build --cross-file /tmp/pico64.txt \
          -Dprefix=/opt/picolibc/x86_64-elf -Dincludedir=include -Dlibdir=lib \
      && ninja -C build && ninja -C build install && cd / && rm -rf /tmp/picolibc /tmp/pico64.txt
  ```
  Zmień też `ENV PREFIX=/opt/i686-nanos` (`Dockerfile:6`) na `ENV PREFIX=/opt/x86_64-nanos`
  i wszelkie `i686-nanos` ścieżki sysrootu na `x86_64-nanos`.

- [ ] **Step 3: Skopiować nagłówki/lib picolibc do sysrootu x86_64-nanos**

  W tej samej warstwie sysrootu Dockerfile’a (lub w `sync-sysroot`), tak jak dla i686:
  ```dockerfile
  RUN cp -R /opt/picolibc/x86_64-elf/include/. /opt/x86_64-nanos/x86_64-nanos/include/ \
   && cp /opt/picolibc/x86_64-elf/lib/*.a       /opt/x86_64-nanos/x86_64-nanos/lib/ || true
  ```

- [ ] **Step 4: Przebudować obraz SDK**

  **[SDK]** Run: `docker build -t nanos-sdk-dev:latest -f ~/Projects/nanos-sdk/Dockerfile ~/Projects/nanos-sdk`
  Expected: build przechodzi; obraz zawiera `x86_64-nanos-gcc` + picolibc x86_64.

- [ ] **Step 5: Commit (nanos-sdk)**

  **[SDK]**
  ```bash
  git -C ~/Projects/nanos-sdk add picolibc-x86_64-elf.txt Dockerfile
  git -C ~/Projects/nanos-sdk commit -m "sdk: build picolibc + sysroot for x86_64-nanos"
  ```

---

## Task 6: [NANOS+SDK] 8b — wstrzyknąć 4 artefakty i smoke-test end-to-end `hello.nxe`

Najpierw aktualizujemy ścieżkę kontraktu w Makefile NanOS (`i686-nanos` → `x86_64-nanos`), potem
budujemy minimalny `hello.nxe` toolchainem SDK i uruchamiamy go na NanOS x86_64 w QEMU. **To
pierwszy realnie weryfikowalny dowód, że dźwignia 4-artefaktowa działa na 64-bit.**

- [ ] **Step 1: Zaktualizować triplet w celach SDK-install Makefile (NanOS)**

  **[NANOS]** W `Makefile` zamień wszystkie wystąpienia ścieżki sysrootu kontraktu z `i686-nanos`
  na `x86_64-nanos` (linie 92/95-98 i analogiczne we wszystkich celach portów — `ping`, `wget`,
  `git`, `inetd`, `httpd`, `openssl`, `dropbear`, …). Wzorzec do zamiany (czterolinijkowy blok
  powtórzony w wielu celach):
  ```make
  cp -R user/libc-glue/include/. "$(SDK_TC)/x86_64-nanos/include/"
  cp kernel/SyscallNr.h          "$(SDK_TC)/x86_64-nanos/include/SyscallNr.h"
  cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/x86_64-nanos/lib/libc.a"
  cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/x86_64-nanos/lib/libc.ndl"
  ```
  oraz `@test -d "$(SDK_TC)/x86_64-nanos/include"` w guardach. (Najszybciej:
  `grep -rl 'i686-nanos' Makefile` → sprawdź ręcznie i podmień; **nie** ruszaj komentarzy
  historycznych, tylko realne ścieżki cp/test.)

- [ ] **Step 2: Odświeżyć sysroot SDK z tego checkoutu**

  **[NANOS]** Wykonaj cztery `cp` z dowolnego celu portowego ręcznie (np. uruchamiając `make ping`
  do momentu refreshu, lub kopiując bezpośrednio):
  ```bash
  SDK_TC=~/Projects/nanos-sdk-work/toolchain
  cp -R user/libc-glue/include/. "$SDK_TC/x86_64-nanos/include/"
  cp kernel/SyscallNr.h          "$SDK_TC/x86_64-nanos/include/SyscallNr.h"
  cp bin/libc.ndl.a              "$SDK_TC/x86_64-nanos/lib/libc.a"
  cp bin/libc.ndl                "$SDK_TC/x86_64-nanos/lib/libc.ndl"
  cp bin/crt0.o bin/nxhdr.o      "$SDK_TC/x86_64-nanos/lib/"
  ```
  Expected: sysroot `x86_64-nanos` ma 64-bitowe `libc.a`/`libc.ndl`/`crt0.o`/`nxhdr.o` + nagłówki.

- [ ] **Step 3: Zbudować i `mknx` minimalny `hello.nxe` toolchainem SDK**

  **[SDK]**
  ```bash
  cat > /tmp/hello.c <<'EOF'
  #include <unistd.h>
  int main(void){ const char *m="hello x86_64-nanos\n"; write(1,m,19); return 0; }
  EOF
  docker run --rm -v /tmp:/t -v ~/Projects/nanos-sdk-work/toolchain:/work/toolchain \
    -w /t nanos-sdk-dev:latest sh -c '
      export PATH=/work/toolchain/bin:$PATH
      x86_64-nanos-gcc hello.c -o hello.elf
      x86_64-nanos-readelf -h hello.elf | grep -E "Class|Machine|Type"'
  ```
  Expected: `Class: ELF64`, `Machine: …X86-64`, `Type: EXEC` (nie `DYN`/PIE — patrz OpenSSL Task 13).
  Następnie `mknx` (z repo NanOS, `tools/mknx`) konwertuje `hello.elf` → `hello.nxe`.

- [ ] **Step 4: Uruchomić `hello.nxe` na NanOS x86_64 w QEMU (headless)**

  **[NANOS]** Skopiuj `hello.nxe` do `bin/`, dołóż do obrazu (np. `/apps/hello`), `make image`,
  bootuj headless (wzorzec CLAUDE.md), z `nsh` wpisz `hello` lub uruchom z `init` i zrób
  `screendump`. Konwersja PPM→PNG przez `sips`.
  Expected: na ekranie `hello x86_64-nanos`. **To zamyka 8a+8b: kontrakt 64-bit działa end-to-end.**

- [ ] **Step 5: Commit (NanOS)**

  **[NANOS]**
  ```bash
  git add Makefile
  git commit -m "build: point SDK contract sysroot at x86_64-nanos (i686-nanos retired)"
  ```

---

## Task 7: [PORT:bash] 8c — bash (zewnętrzny fork)

bash = pierwszy port (powłoka — kamień wyjścia). Budowany z forka `~/Projects/bash-nanos` przez
`nanos/build.sh`, montowanego do `nanos-build` przez cel `make bash`.

- [ ] **Step 1: Przełączyć build forka na x86_64-nanos**

  **[PORT:bash]** W `~/Projects/bash-nanos/nanos/build.sh` zmień `CC`/`--host`/triplet z
  `i686-nanos` na `x86_64-nanos` (oraz wszelkie `i686-nanos-gcc` → `x86_64-nanos-gcc`).
  Jeśli build używa cache cross-compile (`config.cache`) — wyczyść go (`rm -f config.cache`),
  bo trzyma 32-bitowe `ac_cv_sizeof_*`.

- [ ] **Step 2: Zbudować bash**

  **[NANOS]** Run: `make bash`
  Expected: powstaje `~/Projects/bash-nanos/nanos/bash.nxe`, kopiowane do `bin/bash.nxe`.
  Run: `x86_64-elf-readelf -h bin/bash.nxe | grep -E 'Class|Machine'` → ELF64 / X86-64.

- [ ] **Step 3: Zweryfikować w QEMU**

  **[NANOS]** `make image`, boot headless, w `nsh`/`init` uruchom `bash`, wykonaj `echo $((1+1))`
  i `ls`, `screendump`.
  Expected: bash startuje, arytmetyka i `ls` działają (dowód poprawnego LP64 + syscalli).

- [ ] **Step 4: Commit (bash-nanos)**

  **[PORT:bash]**
  ```bash
  git -C ~/Projects/bash-nanos add nanos/build.sh
  git -C ~/Projects/bash-nanos commit -m "nanos: build bash for x86_64-nanos"
  ```

---

## Task 8: [PORT] 8c — grep + vim + bzip2 (czyste rekompilacje)

Trzy dojrzałe porty autotools/make (spec §5.4 #6 — czysta rekompilacja). Budowane w
`~/Projects/nanos-sdk-work/{grep-3.11,vim,bzip2-1.0.8}`; cele `make grep`/`vim`/`bzip2` tylko
kopiują gotowe `.nxe`. **Można robić równolegle** (różne katalogi, brak współdzielonego stanu).

- [ ] **Step 1: grep — rekonfiguracja na x86_64-nanos**

  **[PORT:grep]** W `~/Projects/nanos-sdk-work/grep-3.11`: `make distclean` (lub `rm config.cache`),
  rekonfiguruj cross na `--host=x86_64-nanos CC=x86_64-nanos-gcc` (przez `nanos-port`/manifest),
  `make`. Expected: `src/grep.nxe` jako ELF64.
  **[NANOS]** `make grep` → `bin/grep.nxe`.

- [ ] **Step 2: vim — rekonfiguracja**

  **[PORT:vim]** Analogicznie w `~/Projects/nanos-sdk-work/vim`. vim ma cross-cache
  (`vim_cv_*`) — wyczyść/zaktualizuj 32-bitowe wpisy (rozmiary typów). `make` → `src/vim.nxe`.
  **[NANOS]** `make vim` → `bin/vim.nxe`.

- [ ] **Step 3: bzip2 — prosty Makefile**

  **[PORT:bzip2]** `~/Projects/nanos-sdk-work/bzip2-1.0.8`: `make clean`, `make CC=x86_64-nanos-gcc`,
  `mknx bzip2 → bzip2.nxe`. Brak autotools, więc tylko CC.
  **[NANOS]** `make bzip2` → `bin/bzip2.nxe`.

- [ ] **Step 4: Zweryfikować całą trójkę w QEMU**

  **[NANOS]** `make image`, boot headless; w `nsh`: `grep root /disks/main/nanos/config/passwd`,
  `bzip2 -c /etc/hosts | bzip2 -dc`, `vim --version`. `screendump` każdego.
  Expected: grep zwraca dopasowanie, bzip2 round-trip OK, vim drukuje wersję — bez crashy
  (LP64-clean potwierdzone).

- [ ] **Step 5: Commit (każde repo portu osobno)**

  **[PORT:*]** W każdym z `grep-3.11`/`vim`/`bzip2-1.0.8`:
  ```bash
  git add -A && git commit -m "nanos: rebuild for x86_64-nanos"
  ```
  *(jeśli dany port jest pod kontrolą wersji; manifesty `nxport.toml` z `nanos-sdk-work`.)*

---

## Task 9: [PORT:inetutils] 8c — sieć: ping/wget/telnetd/inetd/ifconfig/traceroute

inetutils + usługi (`inetutils-port`, `inetutils-services-port`, `wget-port`). Sterowane przez
`nanos-port` w kontenerze `nanos-sdk-dev`; cele `make ping`/`wget`/`inetd` odświeżają sysroot
i uruchamiają port. Zależą od działającego stosu TCP/IP (już w kernelu).

- [ ] **Step 1: Zaktualizować manifesty portów na x86_64-nanos**

  **[PORT:inetutils]** W `~/Projects/nanos-sdk-work/{inetutils-port,inetutils-services-port,wget-port}/nxport.toml`
  upewnij się, że CC/host = `x86_64-nanos` (zwykle dziedziczone z toolchaina przez `nanos-port`,
  ale wyczyść `config.cache`/`config.guess`/`config.sub` cache z 32-bitowymi wynikami:
  `rm -f config.cache`).

- [ ] **Step 2: Zbudować ping i wget**

  **[NANOS]** Run: `make ping` (montuje `inetutils-port` do `nanos-sdk-dev`, odświeża sysroot,
  uruchamia `nanos-port`), potem `make wget`.
  Expected: `~/Projects/nanos-sdk-work/inetutils-port/ping.nxe` + `wget-port/wget.nxe` jako ELF64.
  **[NANOS]** Skopiowane do `bin/ping.nxe`, `bin/wget.nxe`.

- [ ] **Step 3: Zbudować usługi (telnetd/inetd/ifconfig/traceroute/telnet)**

  **[NANOS]** Run: `make inetd` (a właściwie cel budujący `inetutils-services-port` — patrz
  `make externals` lista). Expected: `inetd.nxe`/`telnetd.nxe`/`ifconfig.nxe`/`traceroute.nxe`.

- [ ] **Step 4: Zweryfikować sieć w QEMU**

  **[NANOS]** `make image`; boot z NIC (`make run-net` headless lub `-netdev user`); w `nsh`:
  `ifconfig`, `ping -c1 10.0.2.2`, `wget -qO- http://10.0.2.2/`. `screendump`.
  Expected: `ifconfig` pokazuje adres, ping dostaje reply, wget pobiera bajty — TCP/IP + LP64 OK.
  *(Pełny `ping wp.pl` zależy od DNS/uplinku — w QEMU `10.0.2.2` jest gwarantowany.)*

- [ ] **Step 5: Commit (repo portów inetutils)**

  **[PORT:inetutils]** `git add -A && git commit -m "nanos: rebuild inetutils suite for x86_64-nanos"`
  w każdym z trzech katalogów portów.

---

## Task 10: [PORT:openssl] 8c — OpenSSL (retarget Configure)

OpenSSL ma własny perlowy `Configure` (nie autotools). Obecny port używa generycznego targetu
`gcc` + `no-asm`. Dla x86_64 mamy dwie ścieżki (spec §5.4 #4): (A) **zachować `no-asm` + generyczny
`gcc`** — Configure wykryje 64-bit z kompilatora, zero ryzyka asm; (B) włączyć natywny asm przez
target `linux-x86_64`. Rekomendacja: **(A) najpierw** (gwarantowany sukces), (B) jako opcjonalne
przyspieszenie po zazielenieniu.

- [ ] **Step 1: Przełączyć CC na x86_64-nanos w hooku Configure**

  **[PORT:openssl]** W `~/Projects/nanos-sdk-work/openssl-port/hooks/pre_configure.sh` zamień:
  ```sh
  export CC=i686-nanos-gcc
  export AR=i686-nanos-ar
  export RANLIB=i686-nanos-ranlib
  export NM=i686-nanos-nm
  ```
  na `x86_64-nanos-*`. Zachowaj `CFLAGS="-fno-pie"` / `LDFLAGS="-no-pie"` (NanlOS `.nxe` to
  ET_EXEC pod stałą bazą — krytyczne, patrz komentarz w hooku; spójne z decyzją #1).

- [ ] **Step 2: Wybrać target Configure**

  **[PORT:openssl]** Ścieżka (A) — bez zmian w linii `./Configure gcc no-asm …` (generyk wykrywa
  LP64 z `x86_64-nanos-gcc`). Ścieżka (B) opcjonalnie — zamień `gcc` na `linux-x86_64` i usuń
  `no-asm` (perlowe generatory asm dostaną wtedy flavor `elf`/`x86_64`); ale `linux-x86_64`
  zakłada gnu-libc syscalle, więc trzymaj `--with-rand-seed=devrandom` i `no-threads`. **Domyślnie
  zostań przy (A).**

- [ ] **Step 3: Zbudować openssl**

  **[NANOS]** Run: `make openssl` (odświeża sysroot, uruchamia `nanos-port` → Configure+make,
  instaluje też `libssl.a`/`libcrypto.a` + nagłówki do sysrootu dla Dropbear).
  Expected: `~/Projects/nanos-sdk-work/openssl-port/openssl.nxe` ELF64; `bin/openssl.nxe`.

- [ ] **Step 4: Zweryfikować w QEMU**

  **[NANOS]** `make image`, boot headless; w `nsh`:
  `openssl version`, `openssl dgst -sha256 /etc/hosts`, oraz (jeśli sieć) `openssl s_client
  -connect 10.0.2.2:443` przeciw testowemu serwerowi. `screendump`.
  Expected: poprawna wersja + hash (CSPRNG + bignum LP64 OK).

- [ ] **Step 5: Commit (openssl-port)**

  **[PORT:openssl]** `git -C ~/Projects/nanos-sdk-work/openssl-port add hooks/pre_configure.sh &&
  git -C ~/Projects/nanos-sdk-work/openssl-port commit -m "nanos: build OpenSSL for x86_64-nanos"`

---

## Task 11: [PORT:dropbear] 8c — Dropbear (SSH, zależy od OpenSSL/libtomcrypt)

Dropbear linkuje przeciw świeżo zbudowanym `libcrypto`/własnemu libtomcrypt; **musi iść po
OpenSSL** (Task 10). Cel `make dropbear`.

- [ ] **Step 1: Przełączyć port na x86_64-nanos + wyczyścić cache**

  **[PORT:dropbear]** W `~/Projects/nanos-sdk-work/dropbear-port`: `nxport.toml` CC/host →
  `x86_64-nanos`; `rm -f config.cache`. Dropbear ma własne `localoptions.h` — bez zmian
  arch-specyficznych.

- [ ] **Step 2: Zbudować**

  **[NANOS]** Run: `make dropbear`.
  Expected: `dropbear.nxe` (+ `dbclient`/`dropbearkey` jeśli w porcie) ELF64; do `bin/`.

- [ ] **Step 3: Zweryfikować w QEMU**

  **[NANOS]** `make image`, boot z NIC headless; uruchom `dropbear` jako serwer, z hosta
  `ssh -p <fwd> user@localhost` (port-forward QEMU usermode). `screendump` sesji.
  Expected: handshake SSH dochodzi do shella (bash). *(Znana wada z MEMORY: teardown sesji
  Dropbear lingeruje — nie traktować jako regresji LP64, jeśli sam handshake działa.)*

- [ ] **Step 4: Commit (dropbear-port)**

  **[PORT:dropbear]** `git add -A && git commit -m "nanos: build Dropbear for x86_64-nanos"`

---

## Task 12: [PORT:netsurf] 8c — NetSurf + libnsfb (audyt offsetów framebuffera)

NetSurf to graficzna apka NanWM w osobnym repo `~/Projects/netsurf-nanos`; backend `libnsfb` to
**nasz kod** z arytmetyką offsetów framebuffera — wymaga audytu LP64 (spec §5.4 #5, analogicznie
§4 pkt 6). Cel `make netsurf`.

- [ ] **Step 1: Przełączyć build NetSurf na x86_64-nanos**

  **[PORT:netsurf]** W `~/Projects/netsurf-nanos/scripts/{sync-sysroot.sh,build-all.sh}` ustaw
  triplet/CC na `x86_64-nanos`. NetSurf buduje liczne biblioteki (libwapcaplet, libcss, …) —
  wszystkie muszą iść tym samym CC; wyczyść ich cache build.

- [ ] **Step 2: Audyt arytmetyki offsetów w libnsfb (krytyczne na LP64)**

  **[PORT:netsurf]** Przejrzyj backend NanWM/libnsfb pod kątem:
  ```bash
  grep -rnE '\(int\).*(pitch|stride|y \*|offset)|\(unsigned\)[^;]*addr' \
    ~/Projects/netsurf-nanos/ports
  ```
  Każde `(uint32_t)/(int) y * pitch` lub rzutowanie adresu FB na 32-bit → poszerz do
  `size_t`/`uintptr_t` (przy dużym FB 32-bit przepełnia; adres FB może być >4 GiB).

- [ ] **Step 3: Zbudować**

  **[NANOS]** Run: `make netsurf`.
  Expected: `netsurf.nxe` + `res/` skopiowane do `bin/`; ELF64.

- [ ] **Step 4: Zweryfikować w QEMU (graficznie)**

  **[NANOS]** `make image`, boot z NanWM (framebuffer), uruchom NetSurf na lokalny URL
  (`file://` lub `http://10.0.2.2/`), `screendump` okna.
  Expected: poprawnie wyrenderowana strona (brak smug/przesunięć = audyt offsetów OK).

- [ ] **Step 5: Commit (netsurf-nanos)**

  **[PORT:netsurf]** `git -C ~/Projects/netsurf-nanos add -A && git -C ~/Projects/netsurf-nanos
  commit -m "nanos: build for x86_64-nanos + audit libnsfb framebuffer offsets for LP64"`

---

## Task 13: [PORT:doom] 8c — Doom (zależnie od portu: int-vs-pointer)

Doom — ryzyko zależy od bazy portu (spec §5.4 #5): naiwny Doom zakłada `int==pointer`
(`fixed_t`, rzutowania), nowoczesny Chocolate Doom jest 64-bit-clean. **Najpierw ustal, który
port** jest w użyciu (bundle `/apps/doom`).

- [ ] **Step 1: Zidentyfikować bazę portu Doom**

  **[NANOS]** Run: `git -C bin log -1 2>/dev/null; grep -rn 'chocolate\|fixed_t\|doomtype' \
  ~/Projects/*doom* 2>/dev/null | head` (lub sprawdź źródło portu Doom w `nanos-sdk-work`).
  Expected: ustalenie czy to Chocolate (64-bit-clean → czysta rekompilacja) czy klasyk
  (wymaga audytu `fixed_t`/rzutowań).

- [ ] **Step 2: Audyt LP64 (tylko jeśli klasyczny Doom)**

  **[PORT:doom]** `grep -rnE 'int.*=.*\(int\).*ptr|cast.*pointer|long.*==.*4'` — popraw
  `(int)pointer` na `(intptr_t)`, upewnij się że `fixed_t` to typ stałoszerokościowy (i32).
  Chocolate Doom: krok pomijalny.

- [ ] **Step 3: Zbudować + przełączyć CC na x86_64-nanos**

  **[PORT:doom]** CC/host → `x86_64-nanos`, rebuild. Expected: `doom.nxe` ELF64 + IWAD bundle.
  **[NANOS]** Skopiować do `bin/`/bundle `/apps/doom`.

- [ ] **Step 4: Zweryfikować w QEMU (graficznie)**

  **[NANOS]** `make image`, boot z NanWM, uruchom Doom (IWAD `/disks/main/apps/doom/doom1.wad`),
  `screendump` ekranu tytułowego.
  Expected: tytuł/menu renderuje się poprawnie (LP64 + framebuffer OK).

- [ ] **Step 5: Commit (doom port)**

  **[PORT:doom]** `git add -A && git commit -m "nanos: build Doom for x86_64-nanos"`

---

## Task 14: [NANOS] 8c — integracja: pełny obraz 64-bit + headless QEMU smoke całości

Kryterium wyjścia planu: jeden obraz z 64-bitowymi apkami bootuje i odpala bash/nsh +
reprezentatywny zestaw.

- [ ] **Step 1: Zebrać wszystkie zbudowane porty do `bin/`**

  **[NANOS]** Run: `make externals` (best-effort kopiuje każdy istniejący `<app>.nxe` z jego
  work-dir do `bin/`; pomija niezbudowane). Expected: log `staged N external app(s)` z N≈12.

- [ ] **Step 2: Zbudować obraz**

  **[NANOS]** Run: `make image`. Expected: `disk/image-grub2.img` zawiera 64-bitowe binaria pod
  `/nanos/bin`, `/apps`, link-farm `/bin`.

- [ ] **Step 3: Headless smoke — boot + zestaw poleceń**

  **[NANOS]** Boot headless x86_64 (wzorzec CLAUDE.md, `qemu-system-x86_64 -display none -monitor
  unix:/tmp/qmon,server,nowait`), poprzez PTY/nterm wykonaj sekwencję i screendumpuj:
  ```
  ls /            cat /disks/main/nanos/config/passwd
  grep root /disks/main/nanos/config/passwd
  free            bash -c 'echo $((6*7))'
  vim --version   ping -c1 10.0.2.2     openssl version
  ```
  Expected: każda komenda daje sensowne wyjście, brak triple-fault/GP (`-d int -D /tmp/qlog`,
  `grep -E 'v=0d|v=08' /tmp/qlog` powinno być puste). PNG dla dowodu.

- [ ] **Step 4: e2fsck-clean po zapisach**

  **[NANOS]** Po boocie (apki pisały do `/tmp`/`/etc`/rwtest) sprawdź spójność obrazu:
  `docker run --rm -v "$(pwd)":/src -w /src nanos-build e2fsck -fn disk/image-grub2.img` (na
  właściwej partycji wg MBR). Expected: clean (write-path ext4 + JBD2 nienaruszone na 64-bit).

- [ ] **Step 5: Commit (NanOS — staże artefaktów to build-output, nie commitujemy `.nxe`)**

  **[NANOS]** Jeśli były poprawki integracyjne (Makefile/grub.cfg):
  ```bash
  git add -A
  git commit -m "image: integrate x86_64 app ports; full headless boot smoke green"
  ```

---

## Task 15: [NANOS] Domknięcie planu

- [ ] **Step 1: Potwierdzić brak resztek i686 w ścieżce SDK**

  **[NANOS]** Run: `grep -rn 'i686-nanos' Makefile` — Expected: tylko komentarze historyczne,
  zero aktywnych ścieżek cp/test (te są `x86_64-nanos`).

- [ ] **Step 2: Odhaczyć kamień 8 w specu**

  **[NANOS]** W `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md` §7, dopisać przy
  kamieniu 8: „✅ zrealizowane w plans/2026-06-15-x86_64-plan-8-sdk-app-ports.md (toolchain
  x86_64-nanos + N portów; pełny boot zielony)".

- [ ] **Step 3: Commit**

  **[NANOS]**
  ```bash
  git add docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md
  git commit -m "docs: mark x86_64 milestone 8 (SDK + app ports) done"
  ```

---

## Self-Review

- **Spec coverage:** plan realizuje kamień 8 z §7 i całe §5.3 (kontrakt 4 artefaktów jako dźwignia)
  + §5.4 (#1 model kodu/relokacje spójny z mknx/nx.ld/nanos.h — Task 1 Step 4 + Task 3; #4 OpenSSL
  retarget — Task 10; #5 Doom/NetSurf offset-audit — Task 12/13; #6/#7 reszta = mirror + czyste
  rekompilacje). #2 (`%fs.base`) i #3 (SSE) świadomie pozostawione Planowi 6 (kernel/baseline
  toolchaina) — tu tylko konsumowane.
- **Naming contract:** triplet `x86_64-nanos`, prefix `x86_64-nanos-`, `.nxe` v4 / small / low-2GiB
  / `R_X86_64_64`+`R_X86_64_32S`, `LINK_SPEC=-T nx.ld --emit-relocs` — zgodne z Planami 1/6 i mknx.
- **Decyzja #1 cross-plan:** jawnie weryfikowana (Task 1 Step 4) zanim ruszy userland; `nanos.h`
  (Task 3) tylko odzwierciedla model, nie ustanawia go.
- **Uczciwość weryfikacji:** kroki [SDK]/[PORT:*] wykonują się i commitują w osobnych repo — nie
  udajemy ich weryfikacji z repo NanOS; realny dowód wyjścia to [NANOS] make image + headless QEMU
  (Task 6 hello.nxe, Task 14 pełny smoke). Ścieżki dokładnego tekstu w `config.gcc`/`config.bfd`
  oznaczone jako „dopasuj do sąsiedniego wpisu x86_64-*-elf" (wersjozależne) z weryfikacją grep.
- **Placeholders:** brak — każdy krok ma realną komendę/edycję i oczekiwany wynik.

---

## Zależności

- **Cały Plan 8** startuje dopiero, gdy kernel x86_64 bootuje i odpala `init.nxe` (kamień 6, Plan 6).
- **8a (Task 1)** ⟵ Plan 6: 64-bitowe `user/` → `libc.ndl` + x86_64 `SyscallNr.h` + `mknx`/`nx.ld`
  (decyzja #1). Bez tego 8b/8c są zablokowane.
- **8b (Tasks 2–6)** ⟵ 8a: kontrakt 4 artefaktów musi być 64-bit, by toolchain i smoke `hello.nxe`
  miały przeciw czemu linkować. Task 5 (picolibc) ⟵ x86_64-elf z `docker/Dockerfile` (Plan 1).
- **8c (Tasks 7–13)** ⟵ 8b: każdy port linkuje przeciw sysrootowi `x86_64-nanos`. Wewnątrz 8c:
  bash (7) niezależny; grep/vim/bzip2 (8) równolegle; inetutils (9) ⟵ stos TCP/IP (kernel);
  Dropbear (11) ⟵ OpenSSL (10); NetSurf (12)/Doom (13) ⟵ NanWM/framebuffer (kernel) i niezależne
  od siebie. Integracja (14) ⟵ wszystkie porty, które chcemy w obrazie (best-effort).
