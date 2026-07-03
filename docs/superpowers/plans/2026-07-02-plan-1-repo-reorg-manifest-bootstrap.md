# Plan 1: Porządkowanie repo — manifest + bootstrap + ratunek przepisów portów

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wszystko, co potrzebne do zbudowania NanOS z aplikacjami, jest wersjonowane i odtwarzalne: przepisy portów trafiają z niewersjonowanego `~/Projects/nanos-sdk-work` do repo `nanos-sdk`, NanOS dostaje `manifest.toml` + `scripts/bootstrap.sh`, a `nanos-sdk-work` staje się jednorazowym, odtwarzalnym katalogiem build.

**Architecture:** Model „manifest + bootstrap" (jak Android `repo` / Zephyr `west`): NanOS pozostaje głównym repo; repo-rodzeństwo (nanos-sdk + forki aplikacji) jest klonowane obok przez skrypt bootstrap wg manifestu. Przepisy portów (`nxport.toml`, hooki, `*.install`, patche, pliki dokładane do źródeł) żyją w `nanos-sdk/ports/<nazwa>/`; skrypt `nanos-fetch` materializuje z nich katalogi robocze `$SDK_WORK/<nazwa>-port/` (pobiera tarball po pinowanym URL+SHA256 albo klonuje repo forka, nakłada patche/pliki). Makefile NanOS **nie zmienia ścieżek** — dalej używa `$(SDK_WORK)/<nazwa>-port`, tylko te katalogi są teraz odtwarzalne.

**Tech Stack:** POSIX sh, git, docker, curl/wget, tar, patch, sha256. Zero zmian funkcjonalnych w kernelu/userlandzie.

## Global Constraints

- **Żadnych zmian funkcjonalnych** w kernelu, userlandzie ani w logice budowania portów — tylko przenoszenie plików, nowe skrypty, manifest, dokumentacja.
- **Commity bez żadnej atrybucji Claude/AI** (bez `Co-Authored-By`, bez „Generated with"). Krótkie komunikaty w stylu repo, np. `sdk: ports/ — versioned port recipes (rescue from sdk-work)`.
- **Nie commitować** tarballi, rozpakowanych źródeł, artefaktów build (`*.nxe`, `*.o`, `*.a`, `*.ndl`, `build/`, `config.status/log/cache` itd.).
- Ścieżki pozostają nadpisywalne przez env; domyślne: `SDK_WORK=$(HOME)/Projects/nanos-sdk-work`, `NANOS_SDK=$(HOME)/Projects/nanos-sdk`, `BASH_FORK=$(HOME)/Projects/bash-nanos`, `NETSURF_REPO` (sprawdź wartość w Makefile), `SQLITE_FORK=$(HOME)/Projects/sqlite-nanos`.
- Istniejący przepływ na tym Macu ma **dalej działać bez zmian** (wszystko jest addytywne; niczego z `~/Projects/nanos-sdk-work` nie kasujemy w tym planie).
- Repo `nanos-sdk` jest GPL-3.0-or-later — nowe skrypty w nim dostają nagłówek `# SPDX-License-Identifier: GPL-3.0-or-later`.
- Pracujemy w dwóch repo: **NanOS** (`/Users/johniak/Projects/NanOS`, gałąź robocza — utwórz `feat/repo-reorg` od `main`) i **nanos-sdk** (`~/Projects/nanos-sdk`, commituj na jego bieżącą główną gałąź).

## Stan zastany (fakty zebrane 2026-07-02 — nie odkrywaj ich ponownie)

Katalogi portów w `~/Projects/nanos-sdk-work/` (niewersjonowane!) i co zawierają cennego:

| Katalog | Cenne pliki (do uratowania) | Źródło kodu |
|---|---|---|
| `darkhttpd-port` | `nxport.toml`, `darkhttpd.install` | `dir:darkhttpd` (sprawdź provenance — prawdopodobnie klon git) |
| `dropbear-port` | `nxport.toml`, `hooks/`, `dropbear.install` | `dir:dropbear-src` (wersję ustal z `CHANGES` w środku) |
| `git-port` | `nxport.toml`, `git.install`, **`git-2.54.0/config.mak`** (commitowany plik WEWNĄTRZ drzewa źródeł!) | tarball git 2.54.0 |
| `htop-port` | `nxport.toml`, `hooks/`, `htop.install` | `dir:src` + `htop.tar.xz` (3.5.1) |
| `inetutils-port` | `nxport.toml`, `hooks/`, `ping.install` | tarball inetutils-2.5 |
| `inetutils-services-port` | `nxport.toml`, `hooks/`, `inetd.install` | ten sam tarball inetutils-2.5 |
| `libjpeg-port` | `nxport.toml`, `hooks/` | `dir:src` (ustal: libjpeg czy libjpeg-turbo + wersja) |
| `libpng-port` | `nxport.toml`, `hooks/` | `dir:src` (ustal wersję z `png.h`) |
| `ncprobe-port` | `nxport.toml`, `ncprobe.install`, `src/` (**własny program — całe src/ jest cenne**) | własny kod |
| `ncurses-port` | `nxport.toml`, `hooks/` | `dir:src` — sprawdź, czy `src` to klon `ncurses-nanos` |
| `openssl-port` | `nxport.toml`, `hooks/`, `openssl.install` | tarball openssl-3.0.15 |
| `vim-port` | `nxport.toml`, `hooks/`, `vim.install` | `dir:../vim` — sprawdź, czy `nanos-sdk-work/vim` to klon `vim-nanos` |
| `wget-port` | `nxport.toml`, `wget.install` | tarball wget-1.21.4 |
| `zlib-port` | `nxport.toml`, `hooks/` | `dir:src` (ustal wersję z `zlib.h`) |
| `libdrm-port` | `build.sh`, `cross-nanos64.ini`, `nanos-compat.h`, `hooks/` (bez nxport — własny build.sh) | tarball libdrm-2.4.123 (leży obok: `libdrm-2.4.123.tar.xz`) |
| `mesa-port` | `build.sh`, `cross-nanos64.ini`, `cc-wrap`, `cxx-wrap`, `Dockerfile.mako`, `nanos-compat.h`, `hooks/` | tarball mesa-24.2.8 (leży obok) |
| `cxx-port` | `build-libstdcxx.sh` | tarball gcc-14.2.0 |

Poza katalogami `*-port`, w `nanos-sdk-work` bezpośrednio:

| Katalog | Cenne pliki | Uwagi |
|---|---|---|
| `busybox-1.36.1/` | **`nanos-build.sh`** (cel `make udhcpc`, Makefile odwołuje się wprost) | tarball busybox-1.36.1 leży obok |
| `grep-3.11/`, `toybox-0.8.11/`, `sudo-1.9.15p5/`, `bzip2-1.0.8/` | budowane przez wersjonowany `scripts/nx-port-build.sh` z repo NanOS — sprawdź `diff` względem pristine tarballa, czy w drzewach nie ma dodatkowych plików nanos | tarballe leżą obok |
| `toolchain/` | ARTEFAKT (odtwarzalny) — NIE ratujemy plików binarnych; jedynie sprawdź, czy `bin/gen-conftest-stubs.sh` istnieje też w `nanos-sdk/toolchain/` (powinien) | |
| `qemu-virgl-kosmickrisp/` | artefakt brew — odtwarzalny wg komentarza w `scripts/run64-gl.sh` | zostaje |
| `picolibc/`, `musl-1.2.5/`, `ncurses/`, `vim/` | źródła/klony — sprawdź remote'y klonów | |

Jak Makefile NanOS używa portów (wzorce — NIE zmieniamy ich w tym planie):
- Porty nxport: guard na `$(SDK_WORK)/<x>-port/nxport.toml`, refresh sysrootu z checkoutu, `docker run nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port`, `cp *.nxe bin/`.
- Porty nx-port-build: `docker run nanos-build sh /src/scripts/nx-port-build.sh <app>` na `$(SDK_WORK)/<src-dir>`.
- `make bash` używa `$(BASH_FORK)/nanos/build.sh`; `make netsurf` używa `$(NETSURF_REPO)/scripts/{sync-sysroot,build-all}.sh`; `make sqlite` używa `$(SQLITE_FORK)` (luźny katalog, **bez gita** — naprawiamy w Task 6).

---

### Task 1: Inwentaryzacja + audyt wypchnięcia repo

**Files:**
- Create: `~/Projects/nanos-sdk/ports/README.md`

**Interfaces:**
- Produces: tabela inwentarzowa portów (nazwa → źródło, wersja, URL, typ przepisu), użyta przez Task 2–4.

- [ ] **Step 1: Sprawdź stan push wszystkich repo ekosystemu**

```bash
for r in nanos-sdk bash-nanos vim-nanos ncurses-nanos netsurf-nanos; do
  echo "=== $r"; cd ~/Projects/$r
  git status --short | head -5
  git log --oneline @{upstream}..HEAD 2>/dev/null | head -10 || echo "  (brak upstream dla bieżącej gałęzi!)"
  cd - >/dev/null
done
```

Dla każdego repo z niewypchniętymi commitami albo bez upstreamu: `git push` (po obejrzeniu `git log`, czy nie ma tam śmieci). Jeśli `git status` pokazuje niecommitowane zmiany — obejrzyj je (`git diff`) i **zgłoś użytkownikowi zamiast commitować w ciemno**.

- [ ] **Step 2: Ustal brakujące wersje i provenance źródeł `dir:`**

```bash
# wersje z nagłówków/plików:
grep -m1 ZLIB_VERSION ~/Projects/nanos-sdk-work/zlib-port/src/zlib.h
grep -m1 PNG_LIBPNG_VER_STRING ~/Projects/nanos-sdk-work/libpng-port/src/png.h
grep -m1 -ri "JVERSION\|LIBJPEG_TURBO_VERSION" ~/Projects/nanos-sdk-work/libjpeg-port/src | head -3
head -5 ~/Projects/nanos-sdk-work/dropbear-port/dropbear-src/CHANGES
# czy katalogi źródeł to klony gita (jeśli tak — jakie remote'y):
for d in ~/Projects/nanos-sdk-work/vim ~/Projects/nanos-sdk-work/ncurses \
         ~/Projects/nanos-sdk-work/ncurses-port/src ~/Projects/nanos-sdk-work/darkhttpd-port/darkhttpd \
         ~/Projects/nanos-sdk-work/dropbear-port/dropbear-src; do
  echo "== $d"; git -C "$d" remote -v 2>/dev/null || echo "  (nie git)"
done
```

- [ ] **Step 3: Napisz `~/Projects/nanos-sdk/ports/README.md`**

Zawartość: (a) jedno zdanie czym jest `ports/` (kanoniczne, wersjonowane przepisy portów; katalogi `$SDK_WORK/*-port` to jednorazowe materializacje — edytuj TUTAJ i re-materializuj przez `nanos-fetch`), (b) tabela WSZYSTKICH portów z kolumnami: `port | typ (nxport / nx-port-build / build.sh / fork) | źródło (tarball URL+wersja / repo git) | cel w NanOS Makefile`. Wpisz fakty ustalone w Step 1–2 i tabele ze wstępu tego planu. Kanoniczne URL-e tarballi:

```
grep      https://ftp.gnu.org/gnu/grep/grep-3.11.tar.xz
inetutils https://ftp.gnu.org/gnu/inetutils/inetutils-2.5.tar.xz
wget      https://ftp.gnu.org/gnu/wget/wget-1.21.4.tar.gz
gcc       https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz
openssl   https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz
git       https://mirrors.edge.kernel.org/pub/software/scm/git/git-2.54.0.tar.xz
busybox   https://busybox.net/downloads/busybox-1.36.1.tar.bz2
toybox    https://landley.net/toybox/downloads/toybox-0.8.11.tar.gz
sudo      https://www.sudo.ws/dist/sudo-1.9.15p5.tar.gz
bzip2     https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
htop      https://github.com/htop-dev/htop/releases/download/3.5.1/htop-3.5.1.tar.xz
libdrm    https://dri.freedesktop.org/libdrm/libdrm-2.4.123.tar.xz
mesa      https://archive.mesa3d.org/mesa-24.2.8.tar.xz
zlib/libpng/libjpeg/dropbear/darkhttpd — wg wersji ustalonych w Step 2 (zlib: https://zlib.net/fossils/, libpng: https://downloads.sourceforge.net/libpng/, dropbear: https://matt.ucc.asn.au/dropbear/releases/)
```

**Uwaga:** jeśli tarball o tej samej nazwie leży już w `~/Projects/nanos-sdk-work/`, policz jego SHA256 (`shasum -a 256 <plik>`) i wpisz do tabeli — to jest źródło prawdy (URL służy tylko do ponownego pobrania; przy rozjeździe wygrywa suma lokalnego tarballa).

- [ ] **Step 4: Commit w nanos-sdk**

```bash
cd ~/Projects/nanos-sdk && git add ports/README.md && git commit -m "ports: inventory of all NanOS app ports (sources, versions, recipe types)"
```

---

### Task 2: Skopiuj przepisy portów do `nanos-sdk/ports/<nazwa>/`

**Files:**
- Create: `~/Projects/nanos-sdk/ports/<nazwa>/` dla: `darkhttpd dropbear git htop inetutils inetutils-services libjpeg libpng ncprobe ncurses openssl vim wget zlib libdrm mesa cxx busybox sqlite`

**Interfaces:**
- Consumes: tabela z Task 1.
- Produces: układ `ports/<nazwa>/{nxport.toml,hooks/,*.install,files/,src/}` — Task 3 dołoży `source.toml` i `patches/`, Task 4 będzie z tego materializował.

- [ ] **Step 1: Skopiuj przepisy nxport (pętla)**

```bash
cd ~/Projects/nanos-sdk
for p in darkhttpd dropbear git htop libjpeg libpng ncprobe ncurses openssl vim wget zlib; do
  s=~/Projects/nanos-sdk-work/$p-port
  mkdir -p ports/$p
  cp "$s/nxport.toml" ports/$p/
  [ -d "$s/hooks" ] && cp -R "$s/hooks" ports/$p/
  cp "$s"/*.install ports/$p/ 2>/dev/null || true
done
# inetutils ma inne nazwy katalogów niż port:
mkdir -p ports/inetutils ports/inetutils-services
cp ~/Projects/nanos-sdk-work/inetutils-port/nxport.toml ports/inetutils/
cp -R ~/Projects/nanos-sdk-work/inetutils-port/hooks ports/inetutils/
cp ~/Projects/nanos-sdk-work/inetutils-port/ping.install ports/inetutils/
cp ~/Projects/nanos-sdk-work/inetutils-services-port/nxport.toml ports/inetutils-services/
cp -R ~/Projects/nanos-sdk-work/inetutils-services-port/hooks ports/inetutils-services/
cp ~/Projects/nanos-sdk-work/inetutils-services-port/inetd.install ports/inetutils-services/
```

- [ ] **Step 2: Skopiuj przepisy nie-nxport**

```bash
cd ~/Projects/nanos-sdk
mkdir -p ports/libdrm ports/mesa ports/cxx ports/busybox
for f in build.sh cross-nanos64.ini nanos-compat.h; do cp ~/Projects/nanos-sdk-work/libdrm-port/$f ports/libdrm/; done
cp -R ~/Projects/nanos-sdk-work/libdrm-port/hooks ports/libdrm/ 2>/dev/null || true
for f in build.sh cross-nanos64.ini cc-wrap cxx-wrap Dockerfile.mako nanos-compat.h; do cp ~/Projects/nanos-sdk-work/mesa-port/$f ports/mesa/; done
cp -R ~/Projects/nanos-sdk-work/mesa-port/hooks ports/mesa/ 2>/dev/null || true
cp ~/Projects/nanos-sdk-work/cxx-port/build-libstdcxx.sh ports/cxx/
cp ~/Projects/nanos-sdk-work/busybox-1.36.1/nanos-build.sh ports/busybox/
# ncprobe: własny program — całe źródła są cenne:
cp -R ~/Projects/nanos-sdk-work/ncprobe-port/src ports/ncprobe/src
# git: commitowany config.mak żyje WEWNĄTRZ drzewa źródeł — trafia do files/ z docelową ścieżką:
mkdir -p ports/git/files
cp ~/Projects/nanos-sdk-work/git-port/git-2.54.0/config.mak ports/git/files/config.mak
```

Konwencja `files/`: pliki z `ports/<p>/files/` są kopiowane do korzenia rozpakowanego drzewa źródeł przy materializacji (Task 4). Dopisz to zdanie do `ports/README.md`.

- [ ] **Step 3: Przejrzyj, czy nic binarnego nie wchodzi**

```bash
cd ~/Projects/nanos-sdk && git status --short && find ports -size +200k
```

Oczekiwane: tylko pliki tekstowe (toml/sh/h/ini/install/c). Jeśli coś dużego/binarnego — usuń i popraw kopiowanie.

- [ ] **Step 4: Commit + push w nanos-sdk**

```bash
cd ~/Projects/nanos-sdk && git add ports && git commit -m "ports: versioned recipes rescued from the unversioned sdk-work dir" && git push
```

---

### Task 3: Provenance (`source.toml`) + patche vs pristine dla każdego portu

**Files:**
- Create: `~/Projects/nanos-sdk/ports/<nazwa>/source.toml` (każdy port)
- Create: `~/Projects/nanos-sdk/ports/<nazwa>/patches/nanos.patch` (tylko gdzie diff niepusty)

**Interfaces:**
- Consumes: tabela URL/wersji z Task 1, układ `ports/` z Task 2.
- Produces: format `source.toml` czytany przez `nanos-fetch` (Task 4). Dokładnie te klucze:

```toml
# tarball:
kind = "tarball"
url = "https://ftp.gnu.org/gnu/wget/wget-1.21.4.tar.gz"
sha256 = "<hex>"
dir = "wget-1.21.4"          # nazwa katalogu po rozpakowaniu = czego oczekuje nxport.toml `source = "dir:..."`
# albo fork git:
kind = "git"
url = "git@github.com:johniak/vim-nanos.git"
rev = "main"                  # gałąź lub sha
dir = "../vim"                # gdzie klon ma wylądować WZGLĘDEM katalogu <port>-port (vim: ../vim; ncurses: src)
```

- [ ] **Step 1: Wygeneruj `source.toml` dla portów tarballowych**

Porty tarballowe: `git openssl inetutils inetutils-services wget htop libdrm mesa cxx busybox` + (wg ustaleń Task 1) `zlib libpng libjpeg dropbear darkhttpd`. SHA256 bierz z lokalnego tarballa, jeśli leży w `~/Projects/nanos-sdk-work/`:

```bash
shasum -a 256 ~/Projects/nanos-sdk-work/*.tar.* ~/Projects/nanos-sdk-work/*.tgz ~/Projects/nanos-sdk-work/htop-port/htop.tar.xz
```

Dla tarballi, których nie ma lokalnie (zlib/libpng/libjpeg/dropbear — źródła są rozpakowane bez tarballa): pobierz z kanonicznego URL do `/tmp`, policz SHA256, wpisz. `dir` = dokładnie wartość z `source = "dir:..."` w nxport.toml portu (np. openssl: `openssl-3.0.15`, htop/zlib/libpng/libjpeg/ncurses: `src`, dropbear: `dropbear-src` — wtedy w source.toml dodaj klucz `rename = "dropbear-src"` mówiący materializatorowi, by po rozpakowaniu przemianował katalog).

- [ ] **Step 2: `source.toml` dla portów-forków**

`vim` → `kind="git"`, url forka `vim-nanos`, `dir="../vim"`. `ncurses` → url `ncurses-nanos`, `dir="src"`. `darkhttpd` — jeśli Task 1 wykazał klon gita, analogicznie; jeśli luźne pliki, potraktuj jak ncprobe (skopiuj źródła do `ports/darkhttpd/src/`, `kind="local"`). `ncprobe` → `kind="local"` (źródła w `ports/ncprobe/src`, materializacja = kopia). `bash`, `netsurf`, `sqlite` NIE dostają source.toml — idą przez manifest repo (Task 5/6).

- [ ] **Step 3: Diff każdego rozpakowanego drzewa vs pristine tarball**

Dla każdego portu tarballowego, którego źródła leżą rozpakowane w sdk-work (git, openssl, inetutils×2, htop, dropbear, zlib, libpng, libjpeg, grep, toybox, sudo, bzip2, busybox, libdrm, mesa, wget):

```bash
# wzorzec (przykład: openssl)
cd /tmp && rm -rf pristine && mkdir pristine && cd pristine
tar xf ~/Projects/nanos-sdk-work/<tarball>          # albo pobrany
diff -ruN --exclude='*.o' --exclude='*.a' --exclude='*.nxe' --exclude='*.ndl' \
  --exclude='config.log' --exclude='config.status' --exclude='config.cache' \
  --exclude='.deps' --exclude='autom4te.cache' --exclude='*.Po' --exclude='*.Plo' \
  <pristine-dir> ~/Projects/nanos-sdk-work/openssl-port/openssl-3.0.15 > /tmp/openssl.diff; wc -l /tmp/openssl.diff
```

**Obejrzyj każdy diff.** Trzy kategorie: (a) pusty/tylko artefakty configure → nic; (b) dodane pliki nanos (np. `config.mak` gita — już w `files/`) → upewnij się, że są w `ports/<p>/files/`; (c) rzeczywiste modyfikacje upstreamowych plików → zapisz jako `ports/<p>/patches/nanos.patch` (format `-p1` względem korzenia źródeł; przefiltruj z diffa artefakty generowane). Jeśli diff jest podejrzanie wielki (>2000 linii po filtrach) — nie commituj na ślepo, zgłoś użytkownikowi listę plików.

- [ ] **Step 4: Weryfikacja odtwarzalności na jednym porcie (openssl)**

```bash
cd /tmp && rm -rf recon && mkdir recon && cd recon
tar xf <openssl-tarball>
[ -f ~/Projects/nanos-sdk/ports/openssl/patches/nanos.patch ] && (cd openssl-3.0.15 && patch -p1 < ~/Projects/nanos-sdk/ports/openssl/patches/nanos.patch)
cp ~/Projects/nanos-sdk/ports/openssl/files/* openssl-3.0.15/ 2>/dev/null || true
diff -ruN --exclude=<te same wykluczenia> openssl-3.0.15 ~/Projects/nanos-sdk-work/openssl-port/openssl-3.0.15 | head -30
```

Oczekiwane: brak różnic poza artefaktami build. Jeśli są — brakuje patcha/pliku; wróć do Step 3.

- [ ] **Step 5: Commit + push w nanos-sdk**

```bash
cd ~/Projects/nanos-sdk && git add ports && git commit -m "ports: pinned source provenance (source.toml) + pristine-diff patches" && git push
```

---

### Task 4: Materializator `nanos-fetch`

**Files:**
- Create: `~/Projects/nanos-sdk/port/nanos-fetch` (POSIX sh, wykonywalny)
- Modify: `~/Projects/nanos-sdk/ports/README.md` (sekcja użycia)

**Interfaces:**
- Consumes: `ports/<p>/source.toml` (klucze z Task 3), `ports/<p>/{nxport.toml,hooks/,*.install,files/,patches/,src/}`.
- Produces: komenda `nanos-fetch <port> [--work $SDK_WORK]` tworząca `$SDK_WORK/<port>-port/` dokładnie w układzie, którego oczekuje Makefile NanOS. Plan 2 (`make world`) będzie ją wołał.

- [ ] **Step 1: Napisz skrypt**

```sh
#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# nanos-fetch — materialize a port workdir ($SDK_WORK/<port>-port) from the versioned
# recipe in ports/<port>/: download+verify the pinned tarball (or clone the fork repo),
# apply patches/, drop files/, copy the recipe (nxport.toml, hooks/, *.install).
# Idempotent: an existing source tree is left alone (use --force to redo it).
set -eu
SDK=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORT=${1:?usage: nanos-fetch <port> [--work DIR] [--force]}; shift
WORK=${SDK_WORK:-$HOME/Projects/nanos-sdk-work}; FORCE=0
while [ $# -gt 0 ]; do case "$1" in
  --work) WORK=$2; shift 2;; --force) FORCE=1; shift;; *) echo "unknown arg $1"; exit 2;;
esac; done
R="$SDK/ports/$PORT"; [ -d "$R" ] || { echo "no recipe: $R"; exit 1; }
DEST="$WORK/$PORT-port"; DIST="$WORK/dist"; mkdir -p "$DEST" "$DIST"

toml() { sed -n "s/^$1 *= *\"\(.*\)\".*/\1/p" "$R/source.toml" | head -1; }

if [ -f "$R/source.toml" ]; then
  KIND=$(toml kind); DIR=$(toml dir); RENAME=$(toml rename || true)
  case "$KIND" in
  tarball)
    URL=$(toml url); SHA=$(toml sha256); TB="$DIST/$(basename "$URL")"
    [ -f "$TB" ] || curl -fL -o "$TB" "$URL"
    echo "$SHA  $TB" | shasum -a 256 -c - || { echo "SHA256 mismatch: $TB"; exit 1; }
    # dir may point outside the port dir (e.g. vim's "../vim") — resolve relative to $DEST:
    SRC=$(cd "$DEST" && mkdir -p "$(dirname "$DIR")" && cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
    if [ ! -d "$SRC" ] || [ "$FORCE" = 1 ]; then
      rm -rf "$SRC"; T=$(mktemp -d); tar xf "$TB" -C "$T"
      TOP=$(ls "$T"); mv "$T/$TOP" "$SRC"; rmdir "$T"
      if [ -f "$R/patches/nanos.patch" ]; then (cd "$SRC" && patch -p1) < "$R/patches/nanos.patch"; fi
      [ -d "$R/files" ] && cp -R "$R/files/." "$SRC/"
    fi;;
  git)
    URL=$(toml url); REV=$(toml rev)
    SRC=$(cd "$DEST" && mkdir -p "$(dirname "$DIR")" && cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
    [ -d "$SRC/.git" ] || git clone --branch "$REV" "$URL" "$SRC";;
  local)
    SRC=$(cd "$DEST" && pwd)/$DIR
    [ -d "$SRC" ] && [ "$FORCE" = 0 ] || { rm -rf "$SRC"; cp -R "$R/src" "$SRC"; };;
  esac
fi
# the recipe itself:
[ -f "$R/nxport.toml" ] && cp "$R/nxport.toml" "$DEST/"
[ -d "$R/hooks" ] && { rm -rf "$DEST/hooks"; cp -R "$R/hooks" "$DEST/"; }
for f in "$R"/*.install; do [ -f "$f" ] && cp "$f" "$DEST/"; done
# non-nxport build drivers:
for f in build.sh build-libstdcxx.sh nanos-build.sh cross-nanos64.ini cc-wrap cxx-wrap Dockerfile.mako nanos-compat.h; do
  [ -f "$R/$f" ] && cp "$R/$f" "$DEST/"
done
echo "materialized: $DEST"
```

Uwaga dla wykonawcy: dwa porty łamią schemat `<port>-port` — `busybox` materializuje się do `$WORK/busybox-1.36.1` (Makefile: `BB_DIR`), a `cxx` do `$WORK/cxx-port` z tarballem gcc. Obsłuż `busybox` specjalnie: jeśli `PORT = busybox`, ustaw `DEST="$WORK"` i po rozpakowaniu wrzuć `nanos-build.sh` do `$WORK/busybox-1.36.1/`. Podobnie porty `grep/toybox/sudo/bzip2` (nx-port-build): `DEST="$WORK"`, samo rozpakowanie tarballa + patche — bez plików przepisu. Najprościej: klucz `layout = "workdir-root"` w ich source.toml i `case` w skrypcie.

- [ ] **Step 2: Test na czystym scratch-SDK_WORK (openssl, wymaga toolchainu — podlinkuj istniejący)**

```bash
S=/tmp/sdkwork-test && rm -rf $S && mkdir -p $S
ln -s ~/Projects/nanos-sdk-work/toolchain $S/toolchain
~/Projects/nanos-sdk/port/nanos-fetch openssl --work $S
ls $S/openssl-port          # oczekiwane: nxport.toml hooks openssl.install openssl-3.0.15/
cd /Users/johniak/Projects/NanOS
make ARCH=x86_64 openssl SDK_WORK=$S
ls bin/openssl.nxe          # oczekiwane: istnieje, świeży mtime
```

- [ ] **Step 3: Test drugiego kształtu (grep, layout workdir-root) i forka (ncurses)**

```bash
~/Projects/nanos-sdk/port/nanos-fetch grep --work $S && ls $S/grep-3.11/configure
~/Projects/nanos-sdk/port/nanos-fetch ncurses --work $S && ls $S/ncurses-port/src/configure
make ARCH=x86_64 ncurses SDK_WORK=$S && make ARCH=x86_64 grep SDK_WORK=$S
```

- [ ] **Step 4: Dopisz sekcję „Usage" do `ports/README.md`, commit + push**

```bash
cd ~/Projects/nanos-sdk && git add port/nanos-fetch ports/README.md && git commit -m "port: nanos-fetch — materialize port workdirs from versioned recipes" && git push
```

---

### Task 5: `manifest.toml` + `scripts/bootstrap.sh` w NanOS

**Files:**
- Create: `/Users/johniak/Projects/NanOS/manifest.toml`
- Create: `/Users/johniak/Projects/NanOS/scripts/bootstrap.sh`

**Interfaces:**
- Consumes: nic (punkt wejścia świeżej maszyny).
- Produces: po `./scripts/bootstrap.sh` istnieją: repo-rodzeństwo w `$NANOS_ROOT` (domyślnie katalog nadrzędny checkoutu), obrazy dockera `nanos-build` i `nanos-sdk-dev:latest`. Plan 2 zakłada, że to jest zrobione.

- [ ] **Step 1: Napisz `manifest.toml`**

```toml
# NanOS ecosystem manifest — sibling repos cloned next to this checkout by scripts/bootstrap.sh.
# Override the checkout root with NANOS_ROOT (default: parent directory of this repo).
#
# rev: "" = track the branch tip (day-to-day mode). Set a full SHA to PIN the exact revision
# this NanOS commit is known to work with (do this before tags/releases and whenever a
# cross-repo interface changes — the industry norm: Android repo / west pin revisions).
# The key must be present in every block (the parser is deliberately dumb).

[[repo]]
name = "nanos-sdk"
url = "git@github.com:johniak/nanos-sdk.git"
branch = "main"
rev = ""

[[repo]]
name = "bash-nanos"
url = "git@github.com:johniak/bash-nanos.git"
branch = "master"
rev = ""

[[repo]]
name = "vim-nanos"
url = "git@github.com:johniak/vim-nanos.git"
branch = "master"
rev = ""

[[repo]]
name = "ncurses-nanos"
url = "git@github.com:johniak/ncurses-nanos.git"
branch = "master"
rev = ""

[[repo]]
name = "netsurf-nanos"
url = "git@github.com:johniak/netsurf-nanos.git"
branch = "main"
rev = ""

[[repo]]
name = "sqlite-nanos"
url = "git@github.com:johniak/sqlite-nanos.git"
branch = "main"
rev = ""
```

**Sprawdź gałęzie faktycznie używane** (`git -C ~/Projects/<repo> branch --show-current`) i wpisz realne wartości; powyższe to szablon. `sqlite-nanos` dopiero powstanie w Task 6 — wpisz i tak (bootstrap toleruje 404 z ostrzeżeniem do czasu wykonania Task 6).

- [ ] **Step 2: Napisz `scripts/bootstrap.sh`**

```sh
#!/bin/sh
# bootstrap.sh — first-time setup for a fresh machine (macOS or Linux).
# Clones the sibling repos from manifest.toml next to this checkout and builds
# the two docker images every build path needs. Idempotent — safe to re-run.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=${NANOS_ROOT:-$(dirname "$HERE")}

echo "== host prerequisites =="
for c in git docker python3; do
  command -v "$c" >/dev/null || { echo "MISSING: $c — install it and re-run"; exit 1; }
done
docker info >/dev/null 2>&1 || { echo "docker daemon not running"; exit 1; }
command -v qemu-system-x86_64 >/dev/null || echo "WARN: qemu-system-x86_64 not found — builds work, 'make run64'/smokes won't (install qemu)"

echo "== sibling repos -> $ROOT =="
# flat-toml parse: emit "name url branch rev" per [[repo]] block (rev key is mandatory, may be "")
awk -F'"' '/^name/{n=$2} /^url/{u=$2} /^branch/{b=$2} /^rev/{print n, u, b, $2}' "$HERE/manifest.toml" |
while read -r name url branch rev; do
  if [ -d "$ROOT/$name/.git" ]; then
    if [ -n "$rev" ] && [ "$(git -C "$ROOT/$name" rev-parse HEAD)" != "$rev" ]; then
      echo "  WARN: $name is not at the pinned rev $rev (leaving your checkout alone — sync manually)"
    else echo "  ok: $name"; fi
  else
    echo "  clone: $name (${rev:-$branch})"
    if git clone --branch "$branch" "$url" "$ROOT/$name"; then
      [ -n "$rev" ] && git -C "$ROOT/$name" checkout --quiet "$rev"
    else echo "  WARN: clone failed for $name — fix access and re-run"; fi
  fi
done

echo "== docker images =="
docker image inspect nanos-build >/dev/null 2>&1 || (cd "$HERE" && make docker-image)
docker image inspect nanos-sdk-dev:latest >/dev/null 2>&1 || docker build -t nanos-sdk-dev:latest "$ROOT/nanos-sdk"

echo "== done =="
echo "next: make image64 && make run64      # core system"
echo "      make world                      # everything (see BUILDING.md)"
```

`chmod +x scripts/bootstrap.sh`.

- [ ] **Step 3: Test idempotencji na tej maszynie + test świeżego klonowania**

```bash
cd /Users/johniak/Projects/NanOS && ./scripts/bootstrap.sh        # wszystko "ok:", obrazy istnieją
R=/tmp/nanos-root-test && rm -rf $R && mkdir $R
NANOS_ROOT=$R ./scripts/bootstrap.sh                              # klonuje komplet do /tmp/nanos-root-test
ls $R                                                             # oczekiwane: nanos-sdk bash-nanos vim-nanos ncurses-nanos netsurf-nanos (sqlite-nanos po Task 6)
```

- [ ] **Step 4: Commit w NanOS (gałąź feat/repo-reorg)**

```bash
cd /Users/johniak/Projects/NanOS && git checkout -b feat/repo-reorg main 2>/dev/null || git checkout feat/repo-reorg
git add manifest.toml scripts/bootstrap.sh && git commit -m "bootstrap: ecosystem manifest + one-shot fresh-machine setup script"
```

---

### Task 6: sqlite-nanos jako repo + resztki

**Files:**
- Modify: `~/Projects/sqlite-nanos` (init git, wyczyść artefakty, push)
- Modify: `~/Projects/nanos-sdk/ports/README.md` (dopisek o `~/Projects/git` i klonach w sdk-work)

- [ ] **Step 1: Zrób z sqlite-nanos repo**

```bash
cd ~/Projects/sqlite-nanos && ls        # obejrzyj zawartość
printf '*.nxe\n*.ndl\n*.ndl.a\n*.o\n' > .gitignore
git init -b main && git add -A && git status --short   # OBEJRZYJ listę — mają wejść: sqlite3.c sqlite3.h shell.c nanos/ .gitignore; NIE mają: *.nxe *.ndl*
git commit -m "sqlite-nanos: SQLite amalgamation + NanOS compat (initial import)"
gh repo create johniak/sqlite-nanos --private --source . --push
```

(Jeśli `gh` nie skonfigurowane — utwórz repo w UI GitHuba i `git remote add origin … && git push -u origin main`.) Odnotuj w nagłówku README/commitcie wersję SQLite (z `sqlite3.h`: `SQLITE_VERSION`).

- [ ] **Step 2: Dopisz do `nanos-sdk/ports/README.md` sekcję „Nie-porty"**

Treść: `~/Projects/git` to czysty klon upstreamu git/git — NIE jest częścią ekosystemu (port gita buduje z tarballa; nanos-owy `config.mak` jest w `ports/git/files/`). Klony w `nanos-sdk-work` (`vim`, `ncurses/…`) to materializacje forków — kanoniczne są repa z manifestu. Commit + push nanos-sdk.

- [ ] **Step 3: Weryfikacja końcowa planu**

```bash
# pełna materializacja od zera wszystkich portów nxport (toolchain podlinkowany):
S=/tmp/sdkwork-full && rm -rf $S && mkdir -p $S && ln -s ~/Projects/nanos-sdk-work/toolchain $S/toolchain
for p in zlib ncurses libpng libjpeg openssl inetutils inetutils-services wget git dropbear htop vim darkhttpd ncprobe busybox grep toybox sudo bzip2; do
  ~/Projects/nanos-sdk/port/nanos-fetch $p --work $S || echo "FAIL: $p"
done
# próbka buildów przez świeży workdir:
cd /Users/johniak/Projects/NanOS
make ARCH=x86_64 SDK_WORK=$S ncurses vim ping
```

Oczekiwane: zero `FAIL`, trzy `.nxe` w `bin/`. Każdy fail → wróć do source.toml/patcha danego portu.

- [ ] **Step 4: Zgłoś użytkownikowi podsumowanie** (co uratowane, które porty przeszły test świeżej materializacji, co wymaga jego decyzji — np. dostępy GitHub).

---

## Kolejność i zależności

Task 1 → 2 → 3 → 4 (sekwencyjnie; 4 zależy od formatu z 3). Task 5 może iść równolegle z 3–4. Task 6 na końcu (Step 3 to brama całego planu). Plan 2 (`make world`) startuje dopiero po zamknięciu Task 4 i 5.
