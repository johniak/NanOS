# NanOS „buildable from scratch" — design

**Date:** 2026-07-02
**Status:** approved (design review w rozmowie)
**Scope:** dwa plany wykonawcze — (1) porządkowanie układu repo (manifest + bootstrap), (2) `make world` + BUILDING.md + luki linuksowe. Bez nowych funkcjonalności systemu — tylko przenoszenie plików, skrypty, cele Makefile i dokumentacja.

## Cel / kryterium sukcesu

Świeża maszyna **Linux** z zainstalowanym tylko `git + docker + qemu`:

```
git clone <NanOS> && cd NanOS
./scripts/bootstrap.sh     # klonuje repo-rodzeństwo, buduje obrazy Dockera, sprawdza hosta
make world                 # toolchain SDK + wszystkie porty + image64 z kompletem aplikacji
make run64                 # bootuje pełny desktop w QEMU
```

macOS działa jak dotychczas (build i tak jest w kontenerze). **Windows: nie budujemy** — co najwyżej jedna linia w docs: „użyj WSL2 i instrukcji linuksowej".

## Stan zastany (ustalony w rozpoznaniu 2026-07-02)

- Rdzeń (`make image64`) już buduje się w kontenerze Debiana (`docker/Dockerfile`, marker `/etc/nanos-build`, `Makefile:38`). Składanie obrazu dysku używa wyłącznie narzędzi linuksowych (parted/mtools/e2fsprogs/limine/grub) — na Linuksie działa niemal od razu.
- Jedyne istotne macOS-izmy: kopia źródeł do case-sensitive `/tmp/nanos-ksrc` (nieszkodliwa na Linuksie) oraz `scripts/run64-gl.sh` (brew tap kosmickrisp, `codesign`, `-display cocoa,gl=es`) — do przepisania na Linux.
- Porty (`make bash/vim/git/openssl/...`) zależą od katalogów spoza repo, wszystkie przez zmienne `$(HOME)/Projects/...` (brak literalnych `/Users/johniak`):
  - `nanos-sdk` — wersjonowane, remote na GitHubie; zawiera `build-toolchain.sh`, `port/nanos-port`, Dockerfile obrazu `nanos-sdk-dev`.
  - `nanos-sdk-work` — **NIEwersjonowany katalog roboczy**; zawiera jedyne kopie przepisów portów (`*-port/nxport.toml`, `hooks/`, `*.install`), tarballe źródeł, zbudowany toolchain `i686-nanos`/`x86_64-nanos`, drzewo `qemu-virgl-kosmickrisp`.
  - Forki aplikacji: `bash-nanos`, `vim-nanos`, `ncurses-nanos`, `netsurf-nanos` — wersjonowane, mają remote'y.
  - `sqlite-nanos` — luźne pliki bez gita (amalgamacja + artefakty); `~/Projects/git` — czysty klon upstreamu (port buduje z tarballa w `git-port/`, fork zbędny).
- Docs: brak BUILDING.md; główny `README.md` opisuje zabytkowy kernel i686 sprzed Dockera/Limine; najbliższe prawdy jest `docs/en/x86_64.md §7`.
- `nano-packages` (nap) — poza zakresem (mocno in progress).

## Decyzje

1. **Struktura: manifest + bootstrap** (wzór Android `repo` / Zephyr `west` / Chromium `gclient`). NanOS pozostaje głównym repo; dostaje `manifest.toml` (lista repo-rodzeństwa: URL + gałąź/rev) i `scripts/bootstrap.sh` klonujący je do `../`. Bez submodułów, bez scalania historii.
2. **Przepisy portów przenoszą się do `nanos-sdk/ports/<nazwa>/`** (SDK jest właścicielem narzędzia `nanos-port`). Tarballe źródeł: pinowany URL + SHA256 w przepisie, pobierane na żądanie.
3. **`nanos-sdk-work` degraduje się do jednorazowego, odtwarzalnego katalogu build.** Nic cennego nie ma prawa tam mieszkać.
4. **Kolejność: najpierw porządkowanie (Plan 1), potem instrukcja/world (Plan 2)** — pisanie BUILDING.md przeciwko układowi, który zaraz się zmieni, to podwójna robota; ratunek przepisów portów jest pilny (single point of failure na jednym Macu).
5. **Zakres `make world`:** toolchain SDK + wszystkie porty + NetSurf + stos GL (libdrm+Mesa) jako flagowany pod-target; bez nap.
6. **Brama końcowa („clean-machine build" à la Microsoft):** test świeżej maszyny — czysta instalacja Ubuntu: clone → bootstrap → world → `verify64` zielony. Maszyną bramy i buildów jest dedykowany serwer x86_64 użytkownika (i9-13900K/32 GB), który potem zostaje self-hosted runnerem CI (build rdzenia per push + nightly `world`+`verify64`) i mirrorem pinowanych tarballi źródeł (`NANOS_MIRROR` w nanos-fetch).

## Plan 1 — Porządkowanie (manifest + bootstrap)

1. **Ratunek przepisów portów:** każdy `~/Projects/nanos-sdk-work/*-port/{nxport.toml,hooks/,*.install}` → `nanos-sdk/ports/<nazwa>/`; w przepisach pinowane URL+SHA256 tarballi. Commit + push do nanos-sdk.
2. **`manifest.toml` w NanOS** + `scripts/bootstrap.sh`: klonuje rodzeństwo do `../`, buduje `nanos-build` i `nanos-sdk-dev`, weryfikuje wymagania hosta (git, docker, qemu, python3), idempotentny.
3. **Ścieżki w Makefile/skryptach** nadal `$(HOME)/Projects/...` z możliwością nadpisania — bootstrap może przyjąć inny root (`NANOS_ROOT`).
4. **Sprzątanie:** `sqlite-nanos` → zwykły przepis portu; klon `git` poza manifestem; audyt push wszystkich repo z manifestu.

## Plan 2 — `make world` + BUILDING.md + Linux

1. **`make world`:** obrazy Dockera → toolchain `x86_64-nanos` (via `nanos-sdk/build-toolchain.sh`) → porty w kolejności zależności (zlib → ncurses/openssl/libpng/libjpeg → aplikacje → netsurf; libdrm → mesa jako `make world-gl`) → `image64`. Idempotentne i wznawialne (pomija zbudowane).
2. **Luki linuksowe:** linuksowy wariant `run64-gl.sh` (dystrybucyjny QEMU: `-device virtio-vga-gl -display gtk,gl=on`, bez brew/codesign), przegląd skryptów smoke pod Linuksem (pkill/OVMF już przenośne), instalacja QEMU/OVMF per dystrybucja.
3. **Dokumentacja:** nowy `BUILDING.md` (quickstart ≤10 linii, szczegóły, troubleshooting), wymiana stęchłego `README.md`, sekcja „nowy developer: dzień 1".
4. **Brama:** czysta VM Ubuntu — pełny przebieg + `verify64`; opcjonalny workflow CI.

## Poza zakresem

- nap / nano-packages, natywny Windows, przenoszenie historii repo, zmiany funkcjonalne w kernelu/userlandzie, publikacja binarnych artefaktów.
