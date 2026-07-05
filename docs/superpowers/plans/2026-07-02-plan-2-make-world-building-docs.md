# Plan 2: `make world` + BUILDING.md + luki linuksowe + brama clean-machine

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Świeża maszyna Linux (git+docker+qemu) buduje CAŁY NanOS jedną sekwencją: `./scripts/bootstrap.sh && make world && make run64`; dokumentacja (BUILDING.md) prowadzi nowego developera od zera; jedyny macOS-only skrypt (`run64-gl.sh`) dostaje ścieżkę linuksową; całość potwierdzona testem na czystej VM Ubuntu.

**Architecture:** `make world` to sekwencyjny cel-orkiestrator w istniejącym Makefile: obrazy Dockera → materializacja toolchainu `x86_64-nanos` do `$(SDK_WORK)/toolchain` (nowy cel `sdk-toolchain`, ekstrakcja z obrazu `nanos-sdk-dev` + zasiedlenie sysrootu wg wzorca `sync-sysroot`) → materializacja portów (`nanos-fetch` z Planu 1) → porty w kolejności zależności → `image64`. Build i tak dzieje się w kontenerach, więc Linux vs macOS różni się tylko hostowym QEMU i skryptem GL.

**Tech Stack:** GNU make, POSIX sh, docker, qemu. Zero zmian funkcjonalnych w kernelu/userlandzie.

## Global Constraints

- **WYMAGA ukończonego Planu 1** (`docs/superpowers/plans/2026-07-02-plan-1-repo-reorg-manifest-bootstrap.md`): istnieje `nanos-sdk/ports/` + `nanos-fetch` + `manifest.toml` + `scripts/bootstrap.sh`.
- **Commity bez żadnej atrybucji Claude/AI** (bez `Co-Authored-By`, bez „Generated with").
- Żadnych zmian w istniejących celach portów — `world` tylko je WOŁA. Istniejący przepływ ręczny (`make openssl` itd.) działa bez zmian.
- Wszystkie nowe cele Makefile idą w stylu repo: komentarz-nagłówek wyjaśniający PO CO, `.PHONY`, zmienne `?=`.
- `world` = wyłącznie x86_64 (i686 jest zamrożone). GL/Mesa to osobny cel `world-gl` (eksperyment, nie w `world`).
- macOS pozostaje wspierany; Windows tylko jako jedna linia w docs („użyj WSL2 + instrukcja linuksowa"). 
- Gałąź robocza w NanOS: `feat/make-world` (od gałęzi z Planem 1 lub od main po jego merge'u).

## Podział ról: co odpalam na MacBooku, co na serwerze

Docelowy workflow po wykonaniu tego planu (ta sama tabela MUSI trafić do BUILDING.md — Task 4):

| Czynność | Gdzie | Jak |
|---|---|---|
| Edycja kodu, git, IDE | MacBook | jak dotąd |
| Build rdzenia / portów w pętli deweloperskiej | **serwer** | `scripts/remote-build.sh [image64]` (Task 9); lokalny `make image64` na Macu dalej działa, tylko wolniej |
| Uruchamianie i klikanie w desktop (`make run64`) | MacBook | obraz wraca z serwera automatycznie po remote-build |
| GL/virgl (`scripts/run64-gl.sh`) | MacBook | kosmickrisp QEMU (gałąź Darwin); gałąź Linux czeka na maszynę z GUI |
| Pełny build wszystkiego (`make world`) | **serwer** | `scripts/remote-build.sh world` |
| Gate'y (`verify64`, `smoke-*`) | **serwer** (headless, pod globalnym lockiem) | `scripts/remote-build.sh verify64`; na Macu nadal możliwe lokalnie przed pushem |
| CI: build rdzenia per push/PR + nightly `world`+`verify64` | **serwer** (self-hosted runner) | automat, Task 7 |
| Brama clean-machine | **serwer** (świeża VM na nim) | Task 6 Step 2 |
| Mirror tarballi źródeł | **serwer** | nginx na :8090, Task 8 |
| Test na realnym sprzęcie (Dell Latitude) | fizycznie u Ciebie | obraz na USB — jak dotąd |

## Fakty zebrane 2026-07-02 (nie odkrywaj ponownie)

- Makefile jest dwustronny: strona HOST (bez `/etc/nanos-build`) opakowuje wszystko w `docker run nanos-build`; strona KONTENER (`Makefile:1044+`) kompiluje naprawdę. Obraz `nanos-build` z `docker/Dockerfile` ma toolchainy `i686-elf`/`x86_64-elf` + picolibc + Limine + mtools/e2fsprogs/parted — składanie obrazu dysku jest w 100% linuksowe.
- Porty nxport wołają `docker run nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port` z zamontowanym `$(SDK_TC)=$(SDK_WORK)/toolchain` jako `/work/toolchain`; **używany toolchain to ten HOSTOWY katalog**, nie ten w obrazie. Obraz `nanos-sdk-dev` (z `~/Projects/nanos-sdk/Dockerfile`) ma jednak w środku zbudowany `/opt/x86_64-nanos` (binutils+gcc x86_64-nanos) i `/opt/picolibc/x86_64-elf` — to z niego wyciągniemy toolchain na hosta.
- Zasiedlanie sysrootu i686 opisuje `~/Projects/nanos-sdk/sysroot/sync-sysroot` (kopiuje nagłówki picolibc + libc-glue, odpala `posix-hosted-patch.sh`, kopiuje link-bity z checkoutu NanOS, buduje mknx, zakłada wrapper `conftest-strict` + `gen-conftest-stubs.sh`). Dla x86_64 te kroki wykonano ręcznie (commit e9cf56a w nanos-sdk) — Task 1 je skryptuje. Uwaga: cele portów w NanOS Makefile i tak przy KAŻDYM buildzie odświeżają nagłówki libc-glue, `SyscallNr.h`, `nx-dllimport.h`, `libc.a/libc.ndl`, `crt0.o/nxhdr.o` i `x86_64-nanos-mknx` (z `bin/mknx64`) — sysroot musi więc być tylko „wystarczająco kompletny", by configure przeszło: nagłówki picolibc po posix-hosted-patch + wrapper gcc + `gen-conftest-stubs.sh` w `toolchain/bin/`.
- `libtinfo.a`/`libncurses.a` w sysroocie NIE są częścią toolchainu — wytwarza je `make ARCH=x86_64 ncurses` (dlatego ncurses idzie przed vim/htop).
- Kolejność zależności portów: `zlib → ncurses → libpng → libjpeg → openssl` (biblioteki), potem aplikacje: `grep toybox sudo bzip2 vim htop sqlite git ping wget inetd httpd dropbear udhcpc bash`, na końcu `netsurf` (używa libnw + własnych skryptów z `$(NETSURF_REPO)`). `udhcpc` używa `$(SDK_WORK)/busybox-1.36.1/nanos-build.sh`. `httpd` = cel darkhttpd (sprawdź dokładną nazwę celu: `grep -n '^httpd:' Makefile`).
- `make externals` NIE buduje — tylko kopiuje gotowe `.nxe` do `bin/` (po `make clean`).
- `scripts/run64-gl.sh` jest macOS-only: QEMU z tapu kosmickrisp w `$HOME/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp`, ad-hoc `codesign` dylibów brew, `-display cocoa,gl=es`. Stock QEMU zostaje domyślny (tap nie ma SLIRP).
- `scripts/smoke-uefi.sh` już szuka OVMF w `/opt/homebrew/share/qemu/edk2-*.fd` ORAZ `/usr/share/OVMF/OVMF_{CODE,VARS}.fd` — jest gotowy na Linux.
- Root `README.md` opisuje zabytkowy kernel i686 sprzed Dockera/Limine — do wymiany. Kanoniczna tabela celów buildowych: `docs/en/x86_64.md` §7.

---

### Task 1: Cel `make sdk-toolchain` — odtwarzalny toolchain x86_64-nanos na hoście

**Files:**
- Create: `~/Projects/nanos-sdk/sysroot/setup-toolchain` (POSIX sh, wykonywalny)
- Modify: `/Users/johniak/Projects/NanOS/Makefile` (nowy cel `sdk-toolchain` po stronie HOST, obok innych celów portowych)

**Interfaces:**
- Consumes: obraz `nanos-sdk-dev:latest` (bootstrap), `nanos-sdk/sysroot/posix-hosted-patch.sh`, `nanos-sdk/toolchain/{conftest-strict-wrapper.sh,gen-conftest-stubs.sh}`.
- Produces: katalog `$(SDK_WORK)/toolchain` z `bin/x86_64-nanos-{gcc,ld,as,ar,nm,...}`, `bin/gen-conftest-stubs.sh`, `x86_64-nanos/{include,lib}` gotowy dla KAŻDEGO celu portowego. `make world` (Task 2) woła `sdk-toolchain` jako pierwszy krok.

- [ ] **Step 1: Napisz `setup-toolchain` w nanos-sdk**

Skrypt uruchamiany WEWNĄTRZ kontenera `nanos-sdk-dev` z hostowym `$SDK_WORK` zamontowanym pod `/work` i repo nanos-sdk pod `/sdk`:

```sh
#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# setup-toolchain — materialize the host-side $SDK_WORK/toolchain from this image's
# built-in /opt/x86_64-nanos cross toolchain + picolibc, then prepare the sysroot
# (posix-hosted headers + honest-conftest wrapper). The NanOS Makefile port targets
# refresh the checkout-tracked bits (libc-glue headers, libc.a/libc.ndl, crt0/nxhdr,
# mknx) on every build, so this only has to make `configure` viable. Idempotent.
set -eu
TC=/work/toolchain
mkdir -p "$TC"
# 1) the compiler: copy the image's prefix (bin/, libexec/, x86_64-nanos/) into the host tree
cp -a /opt/x86_64-nanos/. "$TC/"
# 2) sysroot headers: picolibc (x86_64-elf machine) + hosted-POSIX adaptation
SYS="$TC/x86_64-nanos"
mkdir -p "$SYS/include" "$SYS/lib"
cp -a /opt/picolibc/x86_64-elf/include/. "$SYS/include/"
cp -a /opt/picolibc/x86_64-elf/lib/.     "$SYS/lib/"
sh /sdk/sysroot/posix-hosted-patch.sh "$SYS/include"
# 3) honest-conftest wrapper (mirrors sync-sysroot's i686 flow)
GCC="$TC/bin/x86_64-nanos-gcc"
if [ -f "$GCC" ] && ! head -1 "$GCC" | grep -q '/bin/sh'; then
  mv "$GCC" "$GCC.real"
  cp /sdk/toolchain/conftest-strict-wrapper.sh "$GCC"; chmod +x "$GCC"
fi
cp /sdk/toolchain/gen-conftest-stubs.sh "$TC/bin/"
echo "toolchain ready: $TC"
```

**Zanim uznasz skrypt za skończony:** porównaj z i686-owym `sync-sysroot` (`~/Projects/nanos-sdk/sysroot/sync-sysroot`) i z zawartością znanego-dobrego `~/Projects/nanos-sdk-work/toolchain/` — sprawdź, czy wrapper na macu faktycznie nazywa się `x86_64-nanos-gcc` + `x86_64-nanos-gcc.real` (`ls ~/Projects/nanos-sdk-work/toolchain/bin | grep real`) i czy `conftest-strict-wrapper.sh` nie jest zaszyty na triplet i686 (`grep i686 ~/Projects/nanos-sdk/toolchain/conftest-strict-wrapper.sh`) — jeśli jest, sparametryzuj wrapper przez nazwę pliku `$0` zamiast kopiować na ślepo.

- [ ] **Step 2: Dodaj cel `sdk-toolchain` do Makefile NanOS (strona HOST, obok portów)**

```make
# sdk-toolchain: materialize the x86_64-nanos cross toolchain + sysroot into $(SDK_WORK)/toolchain
# from the nanos-sdk-dev image (which builds it from source in its Dockerfile). Every `make <port>`
# needs this tree; on a fresh machine run it once after scripts/bootstrap.sh. Idempotent.
.PHONY: sdk-toolchain
sdk-toolchain:
	@mkdir -p "$(SDK_WORK)"
	docker run --rm -v "$(SDK_WORK)":/work -v "$(NANOS_SDK)":/sdk nanos-sdk-dev:latest \
	  sh /sdk/sysroot/setup-toolchain
	@test -x "$(SDK_TC)/bin/x86_64-nanos-gcc" && echo "sdk-toolchain OK: $(SDK_TC)"
```

- [ ] **Step 3: Test na CZYSTYM scratch-SDK_WORK — brama zadania**

```bash
S=/tmp/sdkwork-tc && rm -rf $S && mkdir -p $S
cd /Users/johniak/Projects/NanOS
make sdk-toolchain SDK_WORK=$S
~/Projects/nanos-sdk/port/nanos-fetch openssl --work $S
~/Projects/nanos-sdk/port/nanos-fetch zlib --work $S
make ARCH=x86_64 SDK_WORK=$S zlib openssl
ls -la bin/openssl.nxe
```

Oczekiwane: build przechodzi bez sięgania do starego `~/Projects/nanos-sdk-work`. Typowe braki do dołożenia w `setup-toolchain`, jeśli polegnie: brak `libm.a` (utwórz pusty: `x86_64-nanos-ar rcs .../lib/libm.a`), brakujące pliki cross-cache (są w `nanos-sdk/port/config.cache` — nanos-port bierze je z `/sdk`, nie z toolchainu). Diffuj drzewo z known-good: `diff <(ls ~/Projects/nanos-sdk-work/toolchain/x86_64-nanos/lib) <(ls $S/toolchain/x86_64-nanos/lib)`.

- [ ] **Step 4: Commity**

```bash
cd ~/Projects/nanos-sdk && git add sysroot/setup-toolchain && git commit -m "sysroot: setup-toolchain — one-shot host toolchain materialization for x86_64-nanos" && git push
cd /Users/johniak/Projects/NanOS && git add Makefile && git commit -m "make sdk-toolchain: materialize the port toolchain from the nanos-sdk-dev image"
```

---

### Task 2: Cele `make world` + `make world-gl`

**Files:**
- Modify: `/Users/johniak/Projects/NanOS/Makefile` (strona HOST; nowa sekcja po celach portowych)

**Interfaces:**
- Consumes: `sdk-toolchain` (Task 1), `nanos-fetch` (Plan 1 Task 4), wszystkie istniejące cele portowe.
- Produces: `make world` — kompletny `disk/image64.img` ze wszystkimi aplikacjami; `make world-gl` — dodatkowo libdrm+mesa (eksperyment).

- [ ] **Step 1: Ustal dokładne nazwy wszystkich celów portowych**

```bash
grep -nE '^(bash|grep|toybox|sudo|vim|htop|sqlite|bzip2|ping|wget|git|inetd|httpd|darkhttpd|libdrm|openssl|dropbear|udhcpc|zlib|ncurses|libpng|libjpeg|netsurf|assets|externals):' Makefile
```

Zanotuj faktyczną nazwę celu http-serwera (httpd vs darkhttpd) i czy istnieje cel `ncprobe`. Listę WORLD_PORTS poniżej dopasuj do wyniku.

- [ ] **Step 2: Dodaj cele do Makefile**

```make
# world: everything, one command. Docker images -> port toolchain -> materialized port workdirs
# (nanos-fetch from the versioned recipes in nanos-sdk/ports) -> libraries -> apps -> netsurf ->
# assets -> a full image64. Sequential on purpose: each step is idempotent, so a failed run is
# resumed by re-running `make world`. x86_64 only (i686 is frozen). GL (libdrm+mesa) is the
# separate, experimental `world-gl`.
WORLD_FETCH := zlib ncurses libpng libjpeg openssl inetutils inetutils-services wget git \
               dropbear htop vim darkhttpd ncprobe busybox grep toybox sudo bzip2
WORLD_LIBS  := zlib ncurses libpng libjpeg openssl
WORLD_APPS  := grep toybox sudo bzip2 vim htop sqlite git ping wget inetd httpd dropbear udhcpc bash
.PHONY: world world-gl
world: docker-image
	@docker image inspect nanos-sdk-dev:latest >/dev/null 2>&1 || { echo "run ./scripts/bootstrap.sh first"; exit 1; }
	$(MAKE) sdk-toolchain
	@for p in $(WORLD_FETCH); do "$(NANOS_SDK)/port/nanos-fetch" $$p --work "$(SDK_WORK)" || exit 1; done
	$(MAKE) ARCH=x86_64 build
	@for t in $(WORLD_LIBS);  do $(MAKE) ARCH=x86_64 $$t || exit 1; done
	@for t in $(WORLD_APPS);  do $(MAKE) ARCH=x86_64 $$t || exit 1; done
	$(MAKE) ARCH=x86_64 netsurf
	$(MAKE) assets
	$(MAKE) image64
	@echo "world OK — boot it: make run64"

world-gl: world
	$(MAKE) ARCH=x86_64 libdrm
	@echo "NOTE: mesa/virgl is experimental (feat/linuxkpi-virtio-gpu in flight) — build it via"
	@echo "      the recipe in $(NANOS_SDK)/ports/mesa (build.sh) once the DRM ABI work lands."
	@echo "run with GL: scripts/run64-gl.sh"
```

Dopasuj listy do wyniku Step 1 (nazwa httpd/darkhttpd; `sqlite` wymaga `$(SQLITE_FORK)` z manifestu; `bash` wymaga `$(BASH_FORK)`; jeśli istnieje cel ncprobe — dodaj do APPS). Jeżeli któryś cel wymaga niestandardowego workdiru (busybox → `$(SDK_WORK)/busybox-1.36.1`), to `nanos-fetch busybox` z Planu 1 już to obsługuje.

- [ ] **Step 3: Przebieg pełny na tej maszynie (macOS) — brama zadania**

```bash
cd /Users/johniak/Projects/NanOS
S=/tmp/sdkwork-world && rm -rf $S && mkdir -p $S
make world SDK_WORK=$S 2>&1 | tee /tmp/world.log
```

Oczekiwane: kończy się `world OK`. To długi build (toolchain w obrazie już jest; porty + kernel ~godzina). Każdy failujący port naprawiaj w jego przepisie (`nanos-sdk/ports/<p>`) — NIE obchodź w `world`. Po sukcesie:

```bash
make run64   # ręczny smoke: desktop wstaje, w terminalu: bash, vim, git --version, ping wp.pl (z run-net), htop
```

- [ ] **Step 4: Weryfikacja zawartości obrazu bez bootowania**

```bash
docker run --rm -v "$PWD":/src -w /src nanos-build \
  debugfs -R "ls /nanos/bin" -o 69206016 disk/image64.img 2>/dev/null | tr -s ' ' '\n' | sort | head -40
```

Oczekiwane: `bash… git… htop… openssl… ping… sqlite3… vim…` (offset 69206016 = partycja NANOS; stała jest w `Makefile:29`).

- [ ] **Step 5: Commit**

```bash
git add Makefile && git commit -m "make world: one-command full build (toolchain + all ports + image64); world-gl stub"
```

---

### Task 3: Linuksowa ścieżka GL — `run64-gl.sh`

**Files:**
- Modify: `/Users/johniak/Projects/NanOS/scripts/run64-gl.sh`

**Interfaces:**
- Consumes: `disk/image64.img`.
- Produces: ten sam skrypt działa na Darwin (bez zmian zachowania) i na Linuksie (dystrybucyjny QEMU z virglem).

- [ ] **Step 1: Zrefaktoruj skrypt na dwie gałęzie po `uname -s`**

Zachowaj CAŁĄ istniejącą logikę macOS (kosmickrisp, codesign, cocoa) w gałęzi `Darwin`. Dodaj gałąź `Linux`:

```sh
case "$(uname -s)" in
Linux)
    # Distro QEMU almost always ships virglrenderer: no taps, no codesign, no ANGLE.
    QEMU_GL=${QEMU_GL:-qemu-system-x86_64}
    command -v "$QEMU_GL" >/dev/null || { echo "no $QEMU_GL on PATH"; exit 2; }
    "$QEMU_GL" -device help 2>/dev/null | grep -q virtio-vga-gl || {
        echo "this qemu lacks virtio-vga-gl (build without virglrenderer?) — install the distro qemu-system-x86 package"; exit 2; }
    DISPLAY_BACKEND=${DISPLAY_BACKEND:-gtk,gl=on}     # fallback: sdl,gl=on
    ;;
Darwin)
    …istniejąca logika bez zmian…
    ;;
esac
```

Wspólny `exec` na końcu zostaje jeden (parametryzowany `QEMU_GL` + `DISPLAY_BACKEND`). Zaktualizuj komentarz-nagłówek skryptu o sekcję Linux.

- [ ] **Step 2: Test na macOS (regresja)**

```bash
scripts/run64-gl.sh & sleep 20 && pkill -f qemu-system-x86_64
tail -5 /tmp/nanos-gl.log     # oczekiwane: boot log jak dotychczas
```

Test linuksowy wykonuje Task 5 (clean-machine). Jeśli nie masz teraz Linuksa z GUI — oznacz w commit-message „Linux branch untested until the clean-machine gate".

- [ ] **Step 3: Commit**

```bash
git add scripts/run64-gl.sh && git commit -m "run64-gl: Linux branch (distro qemu + virtio-vga-gl + gtk,gl=on); macOS path unchanged"
```

---

### Task 4: BUILDING.md + wymiana README.md

**Files:**
- Create: `/Users/johniak/Projects/NanOS/BUILDING.md`
- Modify: `/Users/johniak/Projects/NanOS/README.md` (pełna wymiana treści)

**Interfaces:**
- Consumes: bootstrap + world (działające z Task 1–2).
- Produces: dokument onboardingu; Task 5 wykonuje go DOSŁOWNIE na czystej VM (test dokumentu).

- [ ] **Step 1: Napisz `BUILDING.md`**

Struktura obowiązkowa (pisz zwięźle, komendy dosłowne; sekcje w tej kolejności):

```markdown
# Building NanOS

## TL;DR (fresh machine, Linux or macOS)
    # prerequisites: git, docker (running daemon), qemu, python3
    git clone git@github.com:johniak/NanOS.git && cd NanOS
    ./scripts/bootstrap.sh      # clones sibling repos (manifest.toml) + builds the 2 docker images
    make image64 && make run64  # core system in QEMU (~15 min first time: toolchains build in docker)
    make world && make run64    # EVERYTHING: SDK toolchain + all app ports + full image (~1-2 h first time)

## Prerequisites per OS
   - Linux (Ubuntu/Debian): apt install git docker.io qemu-system-x86 ovmf python3 make
     (add your user to the docker group; log out/in)
   - macOS: brew install qemu; Docker Desktop; Xcode CLT (make/git)
   - Windows: not supported natively — use WSL2 and follow the Linux instructions inside it.

## Repo layout & the manifest
   [NanOS = kernel+userland+image; siblings cloned by bootstrap: nanos-sdk (cross toolchain +
   versioned port recipes in ports/), bash-nanos/vim-nanos/ncurses-nanos/netsurf-nanos/sqlite-nanos
   (app forks). $SDK_WORK (~/Projects/nanos-sdk-work) is a DISPOSABLE build dir — regenerate any
   part of it with nanos-sdk/port/nanos-fetch <port> and `make sdk-toolchain`. Override roots with
   NANOS_ROOT / SDK_WORK / NANOS_SDK env vars.]

## Core build targets
   [tabela przeniesiona/zsynchronizowana z docs/en/x86_64.md §7: image64, run64, run64-smp,
   bringup64, test64, smoke-*, verify64 — jedna linia opisu każdy]

## Ports (external apps)
   [make <port> dla pojedynczego; make world dla wszystkiego; gdzie żyją przepisy
   (nanos-sdk/ports/<name>) i jak edytować port: edit recipe -> nanos-fetch --force -> make <port>]

## Where to run what (laptop vs build server)
   [przenieś tabelę „Podział ról" z nagłówka tego planu — dosłownie, z komendami;
   plus: export NANOS_BUILD_HOST=user@server w ~/.zshrc i codzienna pętla
   edit -> scripts/remote-build.sh -> make run64]

## GL / virgl (experimental)
   [scripts/run64-gl.sh; Linux: distro qemu wystarcza; macOS: kosmickrisp setup — link do
   komentarza w skrypcie; stan: 3D-submit w toku (feat/linuxkpi-virtio-gpu)]

## Real hardware
   [jedna linia + link do docs/en/x86_64.md §8]

## Troubleshooting
   [min. 6 wpisów: docker daemon not running; qemu missing on PATH; nanos-sdk-dev image missing
   (bootstrap); port fails "toolchain not found" (make sdk-toolchain); clone failures (SSH keys);
   OVMF not found for smoke-uefi (apt install ovmf); slow first build (toolchains compile once,
   then docker layer cache); case-insensitive FS note for macOS contributors (KSRC copy is automatic)]
```

Wypełnij każdą sekcję realnymi komendami — bez placeholderów. Tabelę celów przepisz z `docs/en/x86_64.md` §7 (nie wymyślaj nazw).

- [ ] **Step 2: Wymień `README.md`**

Nowa treść (krótka, <60 linii): czym jest NanOS (x86_64, SMP, USB, TCP/IP, desktop nwm, realny sprzęt Dell Latitude 5310 — 2-3 zdania), zrzut/odnośnik do assets jeśli jest, **sekcja Build = 4 linie TL;DR z odesłaniem do BUILDING.md**, odnośnik do `docs/en/README.md` (architektura) i `THIRD_PARTY_LICENSES.md`. Zachowaj z obecnego README informacje licencyjne, jeśli tam są; usuń całą instrukcję i686/debugfs/hdiutil.

- [ ] **Step 3: Samosprawdzenie dokumentu**

Wykonaj po kolei KAŻDĄ komendę z sekcji TL;DR w świeżym klonie na tej maszynie (`git clone /Users/johniak/Projects/NanOS /tmp/nanos-doccheck && cd /tmp/nanos-doccheck && NANOS_ROOT=/tmp/nanos-doccheck-root ./scripts/bootstrap.sh …`). Każda komenda, która nie działa dosłownie tak, jak ją zapisano — popraw dokument albo skrypt.

- [ ] **Step 4: Commit**

```bash
git add BUILDING.md README.md && git commit -m "docs: BUILDING.md (fresh-machine onboarding) + rewrite the pre-docker README"
```

---

### Task 5: Brama clean-machine — czysta VM Ubuntu buduje wszystko

**Files:**
- Create: `docs/superpowers/plans/2026-07-02-clean-machine-verification.md` (protokół z przebiegu)

**Interfaces:**
- Consumes: WSZYSTKO z Task 1–4 + Plan 1.
- Produces: dowód, że BUILDING.md działa na świeżym Linuksie; lista poprawek wprowadzonych w trakcie.

- [ ] **Step 1: Przygotuj czystą VM Ubuntu 24.04**

Na tym Macu (Apple Silicon — VM musi emulować x86_64 albo użyj arm64 Ubuntu + qemu-system-x86_64, co jest OK: build i tak jest w dockerze multiarch? NIE — obraz nanos-build to debian amd64 z crossami x86_64; na arm64 doker zbuduje go przez emulację, wolno ale poprawnie). Najprostsze opcje — wybierz pierwszą dostępną: (a) istniejący linuksowy host/serwer użytkownika, (b) `multipass launch -n nanos-test -c 4 -m 8G -d 60G 24.04`, (c) VM w UTM/OrbStack. **Jeśli żadna niedostępna — zatrzymaj się i poproś użytkownika o maszynę; nie symuluj tej bramy.**

- [ ] **Step 2: Wykonaj BUILDING.md dosłownie**

W VM, jako zwykły user: zainstaluj TYLKO to, co wymienia sekcja „Prerequisites per OS", sklonuj NanOS (uwaga: VM potrzebuje deploy-key/dostępu do prywatnych repo — użyj `gh auth login` albo https+PAT; odnotuj w BUILDING.md, jeśli repa są prywatne), i przejdź: `bootstrap.sh` → `make image64` → `make smoke-x86_64` → `make world` → `make verify64`. Loguj wszystko (`| tee ~/log-N.txt`).

- [ ] **Step 3: Każdą różnicę naprawiaj u źródła**

Zasada: poprawka idzie do repo (skrypt/Makefile/BUILDING.md), commit na gałęzi, `git pull` w VM, krok powtórz. Typowe spodziewane: brak grupy docker, `ovmf` ścieżki na Ubuntu ARM, DNS w docker buildzie, `qemu-system-x86_64 -display` w VM bez GUI (użyj `-nographic`/serial smoke'ów — `verify64` już tak działa).

- [ ] **Step 4: Spisz protokół**

`docs/superpowers/plans/2026-07-02-clean-machine-verification.md`: data, spec VM, czas każdego etapu, wynik `verify64` (pełna lista PASS), lista poprawek wprowadzonych w trakcie z hashami commitów, znane ograniczenia (np. GL w headless VM nieprzetestowane — wymaga hosta z GUI).

- [ ] **Step 5: Commit + zgłoszenie**

```bash
git add docs/superpowers/plans/2026-07-02-clean-machine-verification.md && git commit -m "docs: clean-machine verification protocol (fresh Ubuntu VM builds world + verify64)"
```

Zgłoś użytkownikowi: wynik bramy, czasy, co zostało nieprzetestowane (GL na Linuksie z GUI, jeśli VM była headless).

---

### Task 6: Serwer buildowy (i9-13900K, 32 GB) — provisioning + rola clean-machine

Użytkownik ma dedykowany serwer x86_64 (i9-13900K, 24C/32T, 32 GB RAM). Role: (a) główna maszyna ciężkich buildów, (b) host bramy clean-machine z Task 5 (VM na serwerze zamiast VM na Macu — natywne amd64, bez emulacji), (c) self-hosted runner CI (Task 7), (d) mirror tarballi (Task 8).

**Files:**
- Modify: `/Users/johniak/Projects/NanOS/BUILDING.md` (sekcja „Build server", 10-15 linii)

- [ ] **Step 1: Provisioning (wykonuje użytkownik lub agent przez SSH — ustal dostęp z użytkownikiem)**

Ubuntu Server 24.04 LTS (spójnie z BUILDING.md i debianowym kontenerem builda). Potem dokładnie sekcja „Prerequisites per OS" z BUILDING.md:

```bash
sudo apt update && sudo apt install -y git docker.io qemu-system-x86 ovmf python3 make qemu-kvm
sudo usermod -aG docker,kvm $USER   # re-login po tym
# dysk: sprawdź >=200 GB wolnego na /var/lib/docker + katalogi robocze (docker images + SDK_WORK + VMs)
df -h /var/lib/docker /home
```

- [ ] **Step 2: Pierwszy pełny przebieg wg BUILDING.md — to JEST wykonanie bramy z Task 5**

Na serwerze, dosłownie wg TL;DR: clone → `./scripts/bootstrap.sh` → `make image64` → `make smoke-x86_64` → `make world` → `make verify64`. Serwer jest świeżą maszyną linuksową, więc ten przebieg realizuje Task 5 Step 2–3 (protokół spisz tak samo). Do bramy „czystości" użyj kontenera/VM na serwerze, jeśli hostowi doinstalowano już coś ponad listę prerequisites (`multipass launch -n nanos-clean -c 8 -m 16G -d 80G 24.04`).

- [ ] **Step 3: Zanotuj czasy + dopisz sekcję „Build server" do BUILDING.md**

Treść: adres/rola serwera (bez sekretów), jak odpalić zdalny build (`ssh <server> 'cd NanOS && make world'`), uwaga o KVM: gate'y (`verify64`/smoke) celowo zostają na `-accel tcg,thread=multi` (parytet z macOS i determinizm wyników — NIE zmieniaj tego w Makefile); KVM wolno używać tylko do interaktywnego `run64` na serwerze przez ręczne nadpisanie QEMU flags.

- [ ] **Step 4: Commit**

```bash
git add BUILDING.md && git commit -m "docs: build-server section (provisioning, remote world builds, TCG-for-gates rule)"
```

---

### Task 7: CI na self-hosted runnerze (serwer z Task 6)

**Files:**
- Create: `/Users/johniak/Projects/NanOS/.github/workflows/build.yml`

Self-hosted runner rozwiązuje problem kosztu (ciepły cache Dockera → build rdzenia ~10 min zamiast 30-60). **Uwaga bezpieczeństwa:** self-hosted runner tylko dla prywatnych repo i bez odpalania workflow z forków (repo settings → Actions → „Require approval for all outside collaborators").

- [ ] **Step 1: Zarejestruj runnera na serwerze**

GitHub → repo NanOS → Settings → Actions → Runners → „New self-hosted runner" (linux/x64) — wykonaj wyświetlone komendy (`config.sh --url https://github.com/johniak/NanOS --token …`), potem jako usługa:

```bash
cd ~/actions-runner && sudo ./svc.sh install && sudo ./svc.sh start
```

- [ ] **Step 2: Workflow**

```yaml
name: build
on:
  push: {branches: [main]}
  pull_request: {}                  # pre-merge validation — CI ma łapać złamany build ZANIM wejdzie do main
  workflow_dispatch: {}
  schedule: [{cron: "0 3 * * *"}]   # nightly: pełny world
jobs:
  core:
    runs-on: [self-hosted, linux, x64]
    steps:
      - uses: actions/checkout@v4
      - run: make docker-image      # no-op przy ciepłym cache
      - run: make image64
      - run: make test64
      - uses: actions/upload-artifact@v4
        with: {name: image64, path: disk/image64.img, retention-days: 7}
  world:
    if: github.event_name != 'push'   # nightly/manual only — pełny build jest długi
    needs: core
    runs-on: [self-hosted, linux, x64]
    steps:
      - uses: actions/checkout@v4
      - run: ./scripts/bootstrap.sh   # siblings już sklonowane na serwerze -> "ok:" + no-op
      - run: make world
      - run: make verify64
```

Dopasuj: bootstrap na runnerze potrzebuje dostępu git do repo-rodzeństwa (deploy keys albo https+PAT w `~/.git-credentials` runnera — ustal z użytkownikiem); `NANOS_ROOT`/`SDK_WORK` wskaż na stałe katalogi robocze runnera (env w workflow), żeby cache portów przeżywał między jobami.

- [ ] **Step 3: Zielony przebieg + commit**

Odpal `workflow_dispatch`, sprawdź oba joby. Commit: `ci: core build per push + nightly world/verify64 on the self-hosted runner`.

---

### Task 8: Mirror tarballi źródeł na serwerze + fallback w nanos-fetch

**Files:**
- Modify: `~/Projects/nanos-sdk/port/nanos-fetch` (obsługa `NANOS_MIRROR`)
- Modify: `~/Projects/nanos-sdk/ports/README.md` (sekcja mirror)

Polisa na znikające upstreamy (sourceforge/sourceware/matt.ucc): wszystkie pinowane tarballe leżą też na serwerze; `nanos-fetch` próbuje najpierw mirror (szybciej, LAN), potem kanoniczny URL. SHA256 i tak weryfikuje każdą kopię, więc mirror niczego nie osłabia.

- [ ] **Step 1: Katalog + serwowanie na serwerze**

```bash
sudo mkdir -p /srv/nanos-dist && sudo chown $USER /srv/nanos-dist
# wypełnij z istniejącego cache po pierwszym `make world` (Task 6): cp $SDK_WORK/dist/* /srv/nanos-dist/
docker run -d --restart unless-stopped --name nanos-dist -p 8090:80 \
  -v /srv/nanos-dist:/usr/share/nginx/html:ro nginx:alpine
```

- [ ] **Step 2: Fallback w nanos-fetch**

W miejscu pobierania tarballa zamień pojedynczy `curl` na:

```sh
if [ ! -f "$TB" ]; then
  ok=
  [ -n "${NANOS_MIRROR:-}" ] && curl -fL -o "$TB" "$NANOS_MIRROR/$(basename "$URL")" && ok=1 || true
  [ -n "$ok" ] || curl -fL -o "$TB" "$URL"
fi
```

(SHA256-check zostaje bez zmian tuż za tym.) Test: `NANOS_MIRROR=http://<server>:8090 nanos-fetch wget --work /tmp/sdkwork-mirror --force` — w logu pobranie z mirrora; z ubitym mirrorem — fallback na URL.

- [ ] **Step 3: Commit + push nanos-sdk**

`port: nanos-fetch — NANOS_MIRROR fallback for pinned source tarballs` + dopisek w ports/README.md i BUILDING.md (env `NANOS_MIRROR`).

---

### Task 9: Pętla deweloperska „build na serwerze, run na Macu" — `scripts/remote-build.sh`

Docelowy workflow użytkownika: edycja + `make run64`/GUI na Macu, ciężkie buildy na serwerze i9. Obraz `disk/image64.img` (~320 MB) wraca po LAN w sekundy.

**Files:**
- Create: `/Users/johniak/Projects/NanOS/scripts/remote-build.sh`
- Modify: `/Users/johniak/Projects/NanOS/BUILDING.md` (sekcja „Build server": workflow remote-build)

**Interfaces:**
- Consumes: serwer po Task 6 (bootstrap zrobiony, siblings + SDK_WORK żyją na serwerze).
- Produces: `scripts/remote-build.sh [cel]` — domyślnie `image64`; `remote-build.sh world` dla pełnego builda; po powrocie obrazu lokalne `make run64` działa bez żadnego builda na Macu.

**Wielu użytkowników / wiele gałęzi — trzy hazardy współdzielonego serwera i jak je skrypt rozwiązuje:**
1. **Kolizja drzew:** katalog builda jest per użytkownik (własne konto SSH → własny `$HOME`) i per GAŁĄŹ (slug z `git branch --show-current`) → `~/build/nanos-<gałąź>`. Dwie gałęzie = dwa katalogi z osobnymi ciepłymi cache'ami przyrostowymi.
2. **Wyścig na sysroocie SDK:** cele portów przy KAŻDYM buildzie wpisują `libc.ndl`/`crt0.o`/nagłówki z checkoutu do `$(SDK_WORK)/toolchain` — współdzielony `SDK_WORK` między gałęziami to korupcja buildów. Dlatego każdy katalog builda ma WŁASNY `SDK_WORK=$RDIR/sdk-work` (koszt: ~2 GB i jednorazowe `make sdk-toolchain` per katalog — tanio, bo to ekstrakcja z obrazu, a `world` robi to samo). **DŁUG TECHNICZNY (nie naprawiaj w tym planie, nie utrwalaj jako wzorca):** zgodnie ze sztuką sysroot powinien być niemutowalnym inputem — docelowa naprawa to przekazywanie bitów z checkoutu (libc/crt0/nagłówki libc-glue) jako osobnego overlaya do nanos-port zamiast wpisywania ich do drzewa toolchainu; per-katalogowy `SDK_WORK` to świadome obejście do czasu tej zmiany.
3. **Cele odpalające QEMU** (`verify64`, `smoke-*`, `test64` z gate'ami, `run*`): skrypty smoke sprzątają przez `pkill -9 -f "qemu-system-…"` — ubiłyby cudzego QEMU — i używają stałych portów hostfwd. Dlatego te cele idą pod GLOBALNYM lockiem serwera (`flock /tmp/nanos-qemu.lock`) — najwyżej chwilę poczekasz w kolejce; zwykłe buildy mają tylko lock per katalog.

Obrazy Dockera (`nanos-build`, `nanos-sdk-dev`) i mirror tarballi są bezpiecznie współdzielone (są tylko-do-odczytu z perspektywy builda).

- [ ] **Step 1: Napisz skrypt**

```sh
#!/bin/sh
# remote-build.sh — build on the build server, run on this machine.
# Rsyncs the working tree (uncommitted changes included) to a per-user, per-branch server
# dir, runs make there with a per-dir SDK_WORK, and pulls the disk image back, so a local
# `make run64` boots the fresh build. Concurrency: plain builds lock their own dir; targets
# that launch QEMU (verify64/smoke-*/test64/run*) take a server-global lock, because the
# smoke scripts pkill qemu by name and bind fixed hostfwd ports.
# Env: NANOS_BUILD_HOST=user@server (required), NANOS_BUILD_DIR (override the derived dir).
# Usage: scripts/remote-build.sh [make-target]      # default: image64; e.g. world, verify64
set -eu
HOST=${NANOS_BUILD_HOST:?set NANOS_BUILD_HOST=user@server}
TARGET=${1:-image64}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BRANCH=$(git -C "$HERE" branch --show-current 2>/dev/null || echo detached)
SLUG=$(printf %s "${BRANCH:-detached}" | tr -c 'A-Za-z0-9._\n-' '-')
RDIR=${NANOS_BUILD_DIR:-build/nanos-$SLUG}
# push the tree (incl. .git — cheap after the first sync); never touch server-side artifacts
rsync -az --delete --mkpath \
  --exclude '/bin/' --exclude '/disk/' --exclude '/sdk-work/' --exclude '.DS_Store' \
  "$HERE/" "$HOST:$RDIR/"
case "$TARGET" in
  verify64|smoke-*|test64|run*) LOCK="flock /tmp/nanos-qemu.lock" ;;   # QEMU targets: global
  *)                            LOCK="flock .build.lock" ;;            # builds: per-dir
esac
ssh "$HOST" "cd $RDIR && SDK_WORK=\$PWD/sdk-work $LOCK make $TARGET"
case "$TARGET" in image64|world|world-gl)
  mkdir -p "$HERE/disk"
  rsync -az "$HOST:$RDIR/disk/image64.img" "$HERE/disk/"
  echo "image pulled — boot it: make run64";;
esac
```

`chmod +x scripts/remote-build.sh`. Uwagi dla wykonawcy: `--delete` z wykluczeniami `/bin/`, `/disk/`, `/sdk-work/` zostawia serwerowe artefakty w spokoju (nie kasuje ich, mimo że nie istnieją po stronie Maca); jeśli rsync na macOS nie zna `--mkpath`, zastąp go `ssh "$HOST" "mkdir -p $RDIR"` przed rsynciem; przy pierwszym buildzie portów w nowym katalogu trzeba raz zrobić `scripts/remote-build.sh sdk-toolchain` (albo po prostu `world`, który robi to sam); cele smoke/verify64 wykonują się w całości na serwerze (headless, serial) — nic nie wraca poza kodem wyjścia. Slug gałęzi trzymaj w zgodzie z tym, co zrobi `tr` (np. `feat/linuxkpi-virtio-gpu` → `feat-linuxkpi-virtio-gpu`).

- [ ] **Step 2: Test pełnej pętli z Maca**

```bash
export NANOS_BUILD_HOST=<user@serwer>
scripts/remote-build.sh image64 && make run64          # desktop wstaje z obrazu zbudowanego na serwerze
scripts/remote-build.sh verify64                        # gate'y zdalnie, wynik w terminalu Maca
# izolacja gałęzi: z drugiego checkoutu/gałęzi odpal build równolegle —
# ssh <serwer> ls build/    → osobne katalogi nanos-<gałąź-1>, nanos-<gałąź-2>, żaden build nie psuje drugiego
# globalny lock QEMU: odpal verify64 z dwóch terminali naraz → drugi czeka na flock, oba kończą zielono
```

Zmierz i zanotuj w BUILDING.md czas `remote-build.sh image64` po drobnej edycji (oczekiwanie: rsync sekundy + przyrostowy build na i9 znacznie szybszy niż lokalny docker na Macu).

- [ ] **Step 3: Dopisz workflow do BUILDING.md (sekcja „Build server") + commit**

```bash
git add scripts/remote-build.sh BUILDING.md && git commit -m "remote-build: edit on the laptop, build on the server, pull the image back"
```

---

## Kolejność i zależności

Task 1 → 2 (world potrzebuje sdk-toolchain) → 3 i 4 równolegle → 5+6 (serwer JEST maszyną bramy clean-machine; Task 6 Step 2 wykonuje Task 5) → 9 (zaraz po 6 — to codzienny workflow użytkownika) → 7 → 8. Wszystko po ukończeniu Planu 1.
