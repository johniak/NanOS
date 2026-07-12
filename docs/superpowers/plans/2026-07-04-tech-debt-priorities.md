# Priorytety długu technicznego NanOS — plan odpracowania (audyt 2026-07-04)

> **For agentic workers:** to jest **meta-plan priorytetów**, nie plan wykonawczy pojedynczego
> feature'u. Pakiety P0 mają zadania wykonawcze inline (checkboxy). Pakiety większe (P1/P2)
> wskazują istniejący plan wykonawczy albo kończą się zadaniem „napisz dedykowany plan
> (superpowers:writing-plans)". Realizacja pakietu = superpowers:subagent-driven-development
> lub superpowers:executing-plans na właściwym planie wykonawczym.

**Goal:** Uporządkować i naprawić to, co dziś najbardziej zagraża projektowi (utrata
przepisów buildów, ukryty bug pamięciowy trzymający kernel na -O0, brak CI) i co najmocniej
blokuje cel „daily driver na Dell Latitude 5310" (NVMe, sufit współbieżności linuxkpi).

**Architecture:** Trzy równoległe tory: (1) *ratunkowy* — reprodukowalność builda + fix UB
+ CI; (2) *sprzętowy* — NVMe i kontynuacja kampanii i915 (bez rozpraszania jej); (3)
*strukturalny* — MM (COW/demand paging), kasacja i686, epoll. Quick-winy higieniczne od razu.

**Źródło:** audyt 4 agentów (jądro/MM, sterowniki/FS/sieć/USB/linuxkpi, userland/build/testy,
dokumenty planów) + weryfikacja ręczna (Makefile, `fs/ExtFilesystem.h`, `lib/String.h`, stan gałęzi).

## Global Constraints

- Commity/PR **bez wzmianki o AI** (żadnych trailerów Co-Authored-By/Generated-with).
- Praca na gałęziach `feat/*` / `fix/*`, nigdy na `main`.
- Bramka jakości: `make test` (≥90% lcov) + `make verify64` zielone przed merge.
- Nie rozbrajać kampanii i915 (`feat/i915-dell-gpu`) — pakiety P0/P1 nie dotykają
  `linuxkpi/` ani `external/linux-6.12/` poza miejscami wskazanymi wprost.
- Zasada z ROADMAP §0: nie kasujemy funkcji z roadmapy; przesuwamy między bramkami.

---

## 0. Diagnoza — co znaleziono (skrót)

**Krytyczne (ryzyko utraty pracy / bug poprawności):**

| # | Problem | Dowód |
|---|---|---|
| K1 | **Repo niebudowalne od zera** — przepisy ~20 portów (grep/vim/openssl/mesa/git/…) żyją TYLKO w niewersjonowanym `~/Projects/nanos-sdk-work`; jedna awaria dysku = utrata przepisów. Plany naprawcze z 2026-07-02 w **0% wykonane** (brak `manifest.toml`, `bootstrap.sh`, `make world`, `BUILDING.md`) | `Makefile:91` + ~30 referencji `SDK_WORK`; plany `2026-07-02-plan-{1,2}-*` |
| K2 | **Kernel na -O0** przez latentny OOB-read w rozwiązywaniu ścieżek ext (triple-fault przy -O2 na boot-mount). To bug pamięciowy na KAŻDYM lookupie ścieżki + ~10× kara wydajności całego jądra | `Makefile:1263-1267` (`KOPTFLAGS=` puste); `fs/ExtFilesystem.h:359-393` |
| K3 | **Zero CI** — istnieje świetna suita (111 plików doctest, bramka 90% lcov, verify64 z 16 smoke'ami), ale nic jej nie uruchamia automatycznie | brak `.github/`, `ci/` |

**Blokery celu „Dell daily-driver":**

| # | Problem | Dowód |
|---|---|---|
| D1 | **Brak sterownika NVMe** — natywny dysk Della nieobsługiwany; storage = ATA PIO per-sektor albo USB-MSC per-sektor | `arch/x86_64/drivers/AtaBlockDevice64.cpp:14-28`; `drivers/UsbMscBlockDevice.cpp:5-39` |
| D2 | **linuxkpi = UP-kooperatywny shim na jądrze SMP**: spinlocki/mutexy/RCU to no-opy (`spinlock.h:34`, `mutex.h:13`, `rcupdate.h:23`), brak IRQ (poll-hook), dma-fence częściowo fałszowane (`kpi_fence.c:292`) — sufit poprawności dla i915 | `linuxkpi/include/linux/*`, `linuxkpi/kpi_fence.c` |
| D3 | USB: transfery per-sektor (brak multi-TRB/SG), xHCI polled pod jednym lockiem, band-aid KernelCr3 zamiast ringów w zawsze-identity regionie DMA | `arch/x86_64/drivers/xhci_x86_64.cpp:5,116-143,396` |
| D4 | Brak epoll; `poll`/`select` = busy-poll przez `ioWait()`+rescan; teardown sesji dropbeara zostawia zombie żrące ~½ CPU | `kernel/SyscallDispatch.cpp:584-600`; `docs/pl/crypto.md:75-81` |

**Największy dług strukturalny:**

| # | Problem | Dowód |
|---|---|---|
| S1 | **MM w całości eager**: fork = pełna kopia (brak COW), brak demand paging (`#PF` ring-3 → SIGSEGV, żadnego fault-in), brak page cache/swap, mmap plików = synchroniczny read przy mapowaniu | `kernel/Exec.cpp:291`, `arch/x86_64/cpu/fault_x86_64.cpp:21-68`, `SyscallDispatch.cpp:941-948` |
| S2 | **Okna VA usera 32-bitowe na x86_64** (bump-pointer + first-fit na `unsigned`), globalne okno staging execa pod jednym lockiem (`STAGE_BASE=0x800000`, `g_execLock` serializuje execi między CPU) | `kernel/Exec.cpp:61-75`, `SyscallDispatch.cpp:882-1010` |
| S3 | **Martwe drzewo i686**: `arch/x86` (45 plików) dubluje `arch/x86_64` (56), a domyślny build to wciąż `ARCH ?= x86`; decyzja projektu = x86_64 ZASTĘPUJE i686 | `Makefile:3`, `arch/x86/*` |
| S4 | Monolityczny Makefile 2740 linii; hardcode `$HOME/Projects/...`; współdzielone `/tmp` (kolizje worktree); smoke'i robią `pkill -9 -f qemu` (ubija cudze QEMU) i mają niebounded `sleep 2/3` | `Makefile` passim; `scripts/smoke-*.sh` |
| S5 | Higiena gita: `develop` martwy (59 commitów za `main`), `main` 1 commit niewypchnięty, brak branch-protection dla `legacy/*`+tagu; śmieci w repo: `disk/auxiliary.img` (31 MB), `disk/image.img.{bak,new}`, stage'e GRUB-a | `git status`/ROADMAP §2.2, §9 |

**Mniejsze (lista do sprzątania przy okazji):** nieatomowy `rename` przez temp-name
(`kernel/Syscall.cpp:892`), revoke JBD2 tylko w obrębie jednej transakcji (`fs/ext/Journal.cpp:114`),
BlockCache 32 sloty (`fs/ext/BlockCache.h:24`), martwe wywołania BKL w stubach asm
(`syscall_entry64.S:46`, `irq64.S:82,99`, `isr64.S:87,93`), `OOO_N=4`/`ACCEPT_N=8`/`POOL_N=128`
w TCP, wycieki detached-pthread + brak `pthread_atfork` (`docs/en/threads.md:83-96`),
`git gc`/`git -C` (plan `2026-06-16-git-port-followup.md`), duplikacja struktur fbdev/evdev
w 6 plikach usera, `icxxabi.cpp` FIXME.

**Świadomie NIE ruszamy teraz:** kampania i915 (w toku, własny plan), Vulkan/venus (bramka
GO/NO-GO nieuruchomiona — po i915), liquid-glass (plan UX, po i915), nap/self-hosting (Q8),
audio/touchpad/bateria (Q7), `external/linux-6.12` w gicie (potrzebny kampanii; ewentualna
migracja do submodule po niej).

---

## 1. Priorytetyzacja — kolejność i uzasadnienie

**P0 (natychmiast, dni):**
- **P0-A: Ratunek reprodukowalności** (K1) — jedyny element, którego zwłoka może kosztować
  *utratę* pracy, nie tylko opóźnienie. Plany wykonawcze już istnieją i są zrecenzowane.
- **P0-B: Fix UB ext + kernel -O2** (K2) — bug poprawności o dużym zasięgu, a odblokowuje
  największy pojedynczy zysk wydajności w projekcie. Mały nakład.
- **P0-C: Quick-winy higieny** (S5) — push `main`, decyzja o `develop`, branch protection,
  usunięcie śmieci z `disk/`. Godziny.

**P1 (najbliższe tygodnie):**
- **P1-A: CI** (K3) — po P0-A (świeży klon musi się budować, żeby CI miało sens).
- **P1-B: NVMe** (D1) — największy brakujący klocek sprzętowy Della; niezależny od i915.
- **P1-C: linuxkpi — utwardzenie współbieżności** (D2) — wpiąć do kampanii i915 jako
  jawne zadanie fazy B (nie osobny tor): minimum = udokumentowany i **egzekwowany** invariant
  single-CPU dla ścieżek DRM + realne locki tam, gdzie IRQ/fence wychodzą poza kooperację.

**P2 (kwartał):**
- **P2-A: MM — COW fork + demand paging + page cache** (S1) — największy dług; osobny plan.
- **P2-B: epoll + poll na wait-queue + fix teardown dropbeara** (D4).
- **P2-C: USB — multi-TRB bulk + IRQ + ringi w regionie identity-DMA** (D3, kasuje KernelCr3).
- **P2-D: Kasacja i686 + flip `ARCH?=x86_64`** (S3) — duża redukcja podwójnego utrzymania.
- **P2-E: Rozbiórka Makefile + per-worktree izolacja + de-flake smoke'ów** (S4).

**P3 (przy okazji / continuous):** lista „mniejsze" wyżej — każdy element jako drobny,
samodzielny PR, gdy ktoś jest w okolicy danego kodu.

Zależności: P0-A → P1-A; P0-B niezależny; P1-B niezależny; P2-C po P0-B (żeby nie debugować
DMA na -O0/-O2 mieszance); P2-D po P1-A (CI łapie regresje kasacji); P2-A po P0-B.

---

## 2. Pakiety P0 — zadania wykonawcze

### Task P0-A: Ratunek reprodukowalności builda

**Files:** wg istniejących planów `docs/superpowers/plans/2026-07-02-plan-1-repo-reorg-manifest-bootstrap.md`
i `2026-07-02-plan-2-make-world-building-docs.md` (oba zrecenzowane, 0% wykonane).

**Interfaces:**
- Produces: wersjonowane `nanos-sdk/ports/<name>/source.toml`, `manifest.toml`,
  `scripts/bootstrap.sh`, targety `make sdk-toolchain` / `make world`, `BUILDING.md`.

- [ ] **Step 1 (RATUNEK, przed wszystkim innym): backup niewersjonowanych przepisów.**
  Zanim ruszy właściwy plan, zabezpieczyć stan: `tar czf ~/nanos-sdk-work-backup-$(date +%F).tgz
  -C ~/Projects nanos-sdk-work sqlite-nanos 2>/dev/null` + skopiować archiwum na drugi nośnik/
  chmurę. To odpina ryzyko utraty od tempa wykonania planu.
- [ ] **Step 2:** wykonać Plan 1 (`2026-07-02-plan-1-...`) zadanie po zadaniu
  (subagent-driven-development). Bramka: świeży `$SDK_WORK` odtwarza się przez `nanos-fetch`.
- [ ] **Step 3:** wykonać Plan 2 (`2026-07-02-plan-2-...`). Bramka: czysta maszyna/VM →
  `bootstrap.sh` → `make world` → bootowalny `image64` (dowód w
  `docs/superpowers/plans/clean-machine-verification.md` wg planu).

### Task P0-B: Diagnoza i fix UB w ext path-resolution → kernel -O2

**Files:**
- Modify: `fs/ExtFilesystem.h:359-393` (`getInodeByPath`/`resolvePath`), ew. `lib/String.h`
- Modify: `Makefile:1263-1269` (`KOPTFLAGS`)
- Test: `tests/` (istniejące doctesty fs/ext4 + nowy test adversarialny)

**Interfaces:**
- Produces: kernel budowany `-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks`
  (te same band-aidy co `UOPTFLAGS`, `Makefile:1262`), `verify64` zielone.

**Uwaga diagnostyczna (ustalona w tym audycie):** komentarz w Makefile obwinia
„non-NUL-terminated String buffer", ale **dzisiejszy `lib/String.h` NUL-terminuje każdą
ścieżkę konstrukcji/append** (`String.h:23,41,54,68`). Kandydaci na prawdziwą przyczynę:
(a) `String::operator char*()` zwraca `textArray == nullptr` po domyślnym konstruktorze —
`resolvePath` to łapie (`!p`), ale inne użycia `(char*)path` już nie; (b) `String::operator+`
**mutuje lewy operand** (`String.h:131-142` — `append` na `this`), więc `a + b` niszczy `a`
— przy -O2 inaczej złożone temporaries mogą eksplodować use-after-free; (c) UB może siedzieć
w innym miejscu jądra, a ext-mount jest tylko pierwszą ofiarą. **Dlatego: najpierw
reprodukcja i diagnoza (superpowers:systematic-debugging), nie ślepa poprawka.**

- [ ] **Step 1: Reprodukcja.** W `Makefile` ustawić tymczasowo
  `KOPTFLAGS=-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks`, `make image64`,
  boot w QEMU z `-d int` → potwierdzić triple-fault przy root-mount; zanotować RIP/CR2.
- [ ] **Step 2: Diagnoza na hoście.** Istniejące doctesty ext (`tests/`) zbudować z
  `-O2 -fsanitize=address,undefined` (host `HOST_CXXFLAGS`, `Makefile:2672`) i przegonić;
  dodać test adversarialny: `resolvePath` na ścieżkach bez separatora końcowego, „/", „//",
  komponent 255 znaków, symlink-loop, oraz `getInodeByPath(String())` (nullptr). Jeśli
  sanitizery nic nie łapią — bisect po plikach: `-O2` per-TU (per-plik `KOPTFLAGS` override)
  aż do wskazania winnego TU.
- [ ] **Step 3: Fix.** Wg diagnozy. Jeśli winne są (a)/(b): dodać w `String` gwarancję
  `textArray != nullptr` (pusty string = `""`), naprawić `operator+` żeby nie mutował
  `this` (zwracać nową kopię), i dodać doctesty na te semantyki.
- [ ] **Step 4: Testy hostowe zielone.** `make test` (bramka 90% lcov) — PASS.
- [ ] **Step 5: Flip -O2 na stałe.** `KOPTFLAGS=-O2 -fno-strict-aliasing
  -fno-delete-null-pointer-checks` + zaktualizować komentarz `Makefile:1250-1267`
  (usunąć nieaktualną diagnozę „non-NUL-terminated").
- [ ] **Step 6: Pełna weryfikacja.** `make verify64` — wszystkie smoke'i PASS; boot na
  Dellu z USB (sanity: login + `ls` na ext-root).
- [ ] **Step 7: Commit** (`fix: ext path-resolution UB; kernel builds -O2`), PR na `main`.

### Task P0-C: Higiena gita i repo (quick-winy)

**Files:** brak zmian kodu; `disk/`, `.gitignore`, origin.

- [ ] **Step 1:** `git push origin main` (wisi 1 commit: `e54c789`).
- [ ] **Step 2:** decyzja `develop`: ROADMAP §2.3 przewiduje `develop` jako gałąź
  integracyjną, ale realnie nikt jej nie używa (59 za `main`, 0 przed). Rekomendacja:
  **skasować** `develop` (lokalnie i na origin, jeśli jest) i poprawić ROADMAP §2.3 —
  model de facto to `main` + `feat/*`. Alternatywa (jeśli ma zostać): fast-forward do `main`.
- [ ] **Step 3:** branch protection na origin (GitHub → Settings → Branches / Tags):
  no force-push + no delete dla `main`, `legacy/*` i tagu `original-zero-ai-2026-02-05`
  (ROADMAP §2.2 ⬜; przez `gh api` albo ręcznie).
- [ ] **Step 4:** skasować z gita śmieci: `git rm disk/image.img.bak disk/image.img.new
  disk/auxiliary.img disk/stage1 disk/stage2 disk/e2fs_stage1_5` + wpis `disk/image.img.*`
  do `.gitignore`. (Uwaga: najpierw `git log --oneline -3 -- disk/auxiliary.img` —
  potwierdzić, że nic z bieżących targetów go nie czyta; grep w Makefile.)
- [ ] **Step 5:** skasować zmerdżowane gałęzie lokalne (`feat/rust-explorer`,
  `fix/vt-charset-escape`, `feat/linuxkpi-virtio-gpu`, `feat/nwnote-notepad`,
  `feat/smp`, `feat/linux-user-permissions` — po `git branch --merged main` weryfikacji)
  oraz `wip/gl-t2-fresh-object` (1 commit — sprawdzić czy zawiera coś niescalonego;
  jeśli tak, cherry-pick albo zostawić z adnotacją).
- [ ] **Step 6: Commit** zmian `disk/`+`.gitignore` na krótkiej gałęzi `chore/repo-hygiene`, PR.

---

## 3. Pakiety P1 — zakres i bramki (plany dedykowane)

### Task P1-A: CI (self-hosted runner)

**Depends:** P0-A. **Zakres:** wykonać zadania CI z Planu 2 (2026-07-02, Task 7-9):
runner + workflow `build.yml` (make test + make world + verify64 nightly), mirror tarballi.
**Bramka:** czerwony build na PR blokuje merge; nightly verify64 raportuje.
- [ ] Napisać/uzupełnić plan wykonawczy, jeśli Plan 2 Task 7-9 okaże się za płytki po P0-A.

### Task P1-B: NVMe (+ PCIe ECAM)

**Zakres:** enumeracja PCIe (ECAM/MMCONFIG), sterownik NVMe (admin queue + 1 para I/O SQ/CQ,
MSI single-vector jak w e1000e — wzorzec już przetestowany), `NvmeBlockDevice` za istniejącym
HAL-em `drivers/BlockDevice.h`, GPT już jest. QEMU: `-device nvme`. Real-HW: Dell.
**Bramka:** boot z ext4-root na NVMe w QEMU + smoke `smoke-nvme` w verify64; potem Dell.
- [ ] Napisać dedykowany plan (superpowers:writing-plans) — wzorować sekwencję na planie
  NIC fazy 1 (`2026-06-20-nic-phase1-core-msi-e1000e.md`: core host-testowalny → QEMU → HW).

### Task P1-C: linuxkpi — jawny invariant współbieżności (w ramach kampanii i915)

**Zakres (dopisać do planu i915 fazy B, nie osobny tor):**
1. **Egzekwowanie** dzisiejszego założenia UP-kooperatywnego: assert/panic gdy ścieżka
   DRM wykona się na CPU ≠ pinned albo równolegle (licznik wejść + `smp_processor_id()`),
   zamiast cichej korupcji.
2. Mapa ryzyk na piśmie: które ścieżki i915 (IRQ, fence signaling, workqueue) wychodzą
   poza kooperatywny pump i kiedy potrzebują realnych locków.
3. Decyzja architektoniczna: docelowo realne spinlocki (kernel je ma — `kernel/Spinlock.h`)
   + przejście z poll-hooka na prawdziwe MSI (już jest `request_irq` z MSI po 447de35).
**Bramka:** i915 nie może przejść do „stable na Dellu" (plan 3, Task 10) z no-op lockami
bez co najmniej egzekwowanego invariantu.
- [ ] Dopisać powyższe jako zadanie do `2026-07-01-plan-3-gl-on-dell-i915-iris.md` fazy B.

---

## 4. Pakiety P2 — zakres i bramki (skrót)

- **P2-A MM:** COW fork (bit R/O + refcount ramek + fault-in w `fault_x86_64.cpp`),
  demand paging mmap plików, page cache nad `BlockCache` (dziś 32 sloty). Duży, osobny
  plan; TDD na hoście na `AddressSpace` (wzorzec z planu paging Plan 3). Bramka: fork
  ciężkiego procesu (bash) bez kopii całego AS; `verify64` + smptorture zielone.
- **P2-B epoll/poll:** wait-queue w jądrze (budzenie z `Pipe`/`Socket`/`Console` zamiast
  rescan-loopa `SyscallDispatch.cpp:584-600`), `SYS_epoll_{create1,ctl,wait}`; przy okazji
  domknąć teardown dropbeara (zombie ~½ CPU, `docs/pl/crypto.md:75`) — to samo gniazdo
  self-pipe/select. Bramka: htop 0% CPU w idle przy otwartym sshd; sesja `ssh host cmd`
  zamyka się czysto.
- **P2-C USB:** multi-TRB bulk (naprawia per-sektor `UsbMscBlockDevice.cpp`), przejście na
  MSI-IRQ event ring, alokacja ringów z regionu `[64MiB,1GiB)` identity-w-każdym-AS →
  kasacja `KernelCr3` (`xhci_x86_64.cpp:116-143`). Bramka: `smoke-usb*` + boot z USB na
  Dellu szybszy zauważalnie (benchmark przed/po w planie).
- **P2-D kasacja i686:** flip `Makefile:3` na `ARCH ?= x86_64`, `git rm -r arch/x86`,
  wyplenić `#ifdef`y i686 (`Exec.cpp:73-75`, `arch/include/arch/mmu.h:21` i in.),
  usunąć stuby BKL z asm (`syscall_entry64.S`/`irq64.S`/`isr64.S`) i `g_bkl`. Bramka:
  `make` bez ARGS buduje x86_64; `verify64` zielone; diff tylko-kasujący.
- **P2-E build-infra:** rozbiórka Makefile (host vs kontener vs porty do osobnych .mk),
  per-worktree `IMAGE`/`QMON`/porty hostfwd/kontenery (ROADMAP §2.4 ⬜), smoke'i: kill po
  PID-zie zamiast `pkill -9 -f qemu`, bounded-poll zamiast `sleep 2/3`. Bramka: dwa
  worktree odpalają `verify64` równolegle bez kolizji.

Każdy pakiet P2 przed startem dostaje własny plan (writing-plans) i gałąź.

---

## 5. Czego świadomie NIE robimy teraz

- **Vulkan/venus** — bramka GO/NO-GO z planu 2 (2026-07-01) nieuruchomiona; wraca po i915.
- **Liquid-glass, proporcjonalne fonty w edycji, frame pacing** — UX po stabilizacji GL/i915.
- **`external/linux-6.12` → submodule** — dopiero po zakończeniu kampanii i915 (dziś to jej
  żywy warsztat).
- **WiFi (iwlwifi), audio HDA, touchpad I2C-HID, bateria/ACPI, nap, self-hosting** — zgodnie
  z ROADMAP (Q7/Q8/follow-on po 1.0); kolejność tam zapisana pozostaje w mocy.

## 6. Self-review

- Pokrycie ustaleń audytu: K1→P0-A, K2→P0-B, K3→P1-A, D1→P1-B, D2→P1-C, D3→P2-C,
  D4→P2-B, S1→P2-A, S2→(P2-A zakres: przy fault-in naturalnie 64-bitujemy okna; jeśli nie —
  osobny follow-up w planie P2-A), S3→P2-D, S4→P2-E, S5→P0-C, „mniejsze"→P3. Brak sierot.
- Sprzeczność źródeł rozstrzygnięta: diagnoza „non-NUL-terminated String" z Makefile vs
  fakt, że `String.h` terminuje — stąd P0-B zaczyna od reprodukcji/diagnozy (Step 1-2),
  a nie od poprawki.
