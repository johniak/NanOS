# NanOS — Roadmap „Real Hardware / Daily Driver" (zespół 30 osób, 18–24 mies.)

> Dokument strategiczny — **główny roadmap NanOS**. Stan repo: gałąź `feat/pthread`
> (multiprocessing + basic coreutils ukończone), 2026-06-16. Definiuje cel nadrzędny, model
> pracy zespołu 30 osób, governance repo oraz 8-kwartałowy backbone milestone'ów prowadzący
> NanOS od „działa w QEMU" do „bootuje i działa jako daily-driver na realnym laptopie".
>
> **Urządzenie docelowe: Dell Latitude 5310** (Comet Lake, 2020). Zastępuje wcześniejszy
> cel (HP EliteBook 820 G3 / Skylake); różnice sprzętowe naniesione w macierzy §5 i kwartałach
> §4 — pozycje, których nie potwierdzono dla konkretnego egzemplarza, oznaczono **(do potwierdzenia)**.
>
> Poprzedni „Multiprocessing Roadmap" jest zachowany niżej jako sekcja **„Fundament (i686)"**.
> Powiązane specy: `2026-06-15-x86_64-migration-analysis.md` (szczegóły techniczne portu 64-bit),
> `2026-06-15-pthread-design.md` (wątki).

---

## Fundament (i686) — ukończone

Multiprocessing + baza systemu są zrobione na **i686** (to był poprzedni „Multiprocessing Roadmap
& Status"). To jest grunt, na którym stoi migracja x86_64 z sekcji niżej (która ten i686 **zastępuje**,
przenosząc zdobycze na 64-bit):

| Etap | Co | Status |
|---|---|---|
| 1 | Paging foundation (frame allocator + `CR0.PG`, `AddressSpace`, ring 0) | ✅ |
| (arch refactor) | MI/MD split — x86 pod `arch/x86/{boot,cpu,mm,drivers}` za kontraktami `<arch/...>`, `make check-arch` | ✅ |
| 2 | init w ring 3 + własna przestrzeń adresowa (izolacja przez `#PF`) | ✅ |
| 3 | Scheduler + abstrakcja `task` (preemptywny RR na PIT 1000 Hz) | ✅ |
| 4 | `fork`/`exec`/`wait`/`exit` (model trap-frame + scheduler, eager fork) | ✅ |
| 5 | Signals + job control (kill/signal/sigreturn/sigprocmask, tty, `jobs`/`fg`/`bg`) | ✅ |
| 6 | Graphics — firmware framebuffer (vesafb/fbcon) + Linux `/dev/fb0` | ✅ |
| (synthetic root) | `/` = `SynthFs` (in-memory), dysk pod `/disks/main` | ✅ |
| (shell+coreutils) | `nsh` + verbatim sbase `cat`/`ls` na ported picolibc | ✅ |

Dalej (poza pierwotnym roadmapem, już zrobione na i686): `.ndl`/`.nkext` (Windows-style dynamic
linking + kext), **ext2/ext4 read+WRITE z JBD2**, **pthread** (musl/picolibc), **stos TCP/IP** +
realny ping/wget, **TLS/SSL/SSH** (OpenSSL + Dropbear), **NanWM** (compositor), porty
(bash/vim/grep/git/netsurf/...), oraz **basic coreutils** (`mkdir rmdir rm touch mv cp ln pwd chmod
wc head tail true false env basename dirname`, 2026-06-16). Decyzje projektowe i wewnętrzne detale
i686 — historia w gicie + `docs/superpowers/{specs,plans}/2026-06-07..16*`.

---

## 0. Cel nadrzędny i zasady

**North star:** NanOS bootuje z UEFI i działa jako użyteczny system na **Dell Latitude 5310**
(Comet Lake, wariant i5-10310U = 4 rdzenie / 8 wątków; i3-10110U = 2C/4T — **do potwierdzenia**
egzemplarza). „Działa" = klawiatura, ekran, dysk, sieć, shell, GUI, porty (bash/vim/netsurf) —
na bare-metal, nie tylko w QEMU.

**Co to oznacza dla priorytetów (obiektywne sufity, w kolejności):**

1. **x86_64 (long mode)** — fundament wszystkiego nowoczesnego (NX, >4 GiB, UEFI startuje
   w 64-bit, SMP/APIC zakłada 64-bit). Bez tego reszta to ślepa uliczka.
2. **Firmware/boot realny** — UEFI + GOP + ACPI; dziś tylko Multiboot1/BIOS w QEMU. **Uwaga:**
   Latitude 5310 (Comet Lake, 2020) może być **UEFI Class 3 bez CSM** — wtedy nie ma taniego
   bootstrapu CSM+Multiboot1 i UEFI jest wymagane wcześniej (do potwierdzenia w BIOS).
3. **Stos USB (xHCI)** — żadna realna maszyna nie jest w pełni użyteczna bez USB
   (storage do flashowania, mysz, urządzenia zewnętrzne).
4. **Realny magazyn** — **NVMe (M.2) podstawowy w 5310** + GPT + FAT (partycja ESP); AHCI/SATA
   tylko jeśli egzemplarz ma dysk SATA (do potwierdzenia); dziś tylko ATA PIO.
5. **SMP** — 5310 ma do 4 rdzeni / 8 wątków; dziś wszystko jednoprocesorowe
   („concurrency, not parallelism" wg specu pthread).
6. **Dojrzałość MM + hardening** — COW fork (dziś eager copy), demand paging, page
   cache, W^X/NX, KASLR/ASLR, SMEP/SMAP (wszystko wymaga 64-bit).
7. **Pakiety + głębia POSIX + self-hosting** — `nap`, epoll/poll/timerfd,
   menedżer pakietów, próba kompilacji on-device.

**Zasady prowadzenia (z CLAUDE.md + governance):**
- TDD wszędzie gdzie host-testowalne (≥90% lcov bramka); headless-QEMU verification.
- Praca na gałęziach feature, nigdy na trunku. Commity/PR **bez wzmianki o AI**.
- Utrzymujemy podział MI/MD i `make check-arch`; nowe arch = `arch/x86_64/`.
- **Decoupling:** `make image` nie zależy od aplikacji — kernel doprowadzamy do 64-bit
  zanim tkniemy którykolwiek port.
- **Nie kasujemy funkcji z roadmapy.** Elementy o różnym ryzyku układamy w bramki
  `required` / `proof` / `follow-on`, zamiast usuwać je z planu. Daily-driver 1.0 może
  mieć GOP bez akceleracji i LAN bez WiFi, ale ścieżki `i915`/WiFi/LinuxKPI/self-hosting
  pozostają zaplanowane jako dowody kierunku albo prace po 1.0.

---

## 1. Strategia sekwencjonowania (podejście B)

x86_64 to fundament, ale jego krytyczna ścieżka (~1500–2000 linii) nie zatrudni 30 osób.
Dlatego:

> **Mały elitarny zespół pcha krytyczną ścieżkę x86_64 (boot/paging/przerwania/syscall).
> Równolegle reszta buduje komponenty MI / host-testowalne (rdzeń USB, virtio, COW,
> dokończenie pthread, audyt blokad SMP, toolchain), które integrują się gdy 64-bit
> wstanie.** Zero podwójnej pracy — komponenty MI są architektonicznie niezależne.

Odrzucone: (A) „x86_64 hard-first, reszta czeka" (≈25 osób bezczynnych 2 kwartały);
(C) „sprzęt na i686, port na końcu" (podwójna praca, sprzeczne z decyzją o zastąpieniu).

### 1.1. Strategia ponownego użycia sterowników Linuksa (warstwa LinuxKPI)

Pisanie od zera sterowników do Intel UHD Graphics (GPU, Comet Lake), Wi-Fi 6 AX201 (WiFi) czy
nowych NIC-ów to praca rzędu lat. **FreeBSD rozwiązuje to dwoma warstwami; bierzemy z nich wzorzec:**

- **LinuxKPI (kernel-space)** — shim implementujący API jądra Linuksa (`struct device`,
  PCI API, DMA API, workqueues, completions, mutexy/spinlocki, `kmalloc`, `ioremap`,
  IRQ threads, `dma-buf`), o który **rekompiluje się źródła sterowników Linuksa**
  (np. DRM `i915`, `iwlwifi`). To jest to, co pozwala FreeBSD używać linuksowego
  `drm-kmod`. **To jest to, czego dotyczy ta prośba.**
- (Linuxulator = user-space ABI to osobny temat — nie dotyczy sterowników; pomijamy.)

**Decyzja strategiczna:** NanOS dostaje **model urządzeń ukształtowany pod Linuksa** —
prymitywy SMP (Strumień C: mutexy/spinlocki), PCIe + MSI/MSI-X (E), DMA API i IRQ
threads projektujemy **z semantyką zgodną z Linuksem**, tak by późniejszy shim LinuxKPI
był cienki, a nie wymagał przerabiania rdzenia. To **nie** jest istniejący mechanizm
`.nkext` (to ładowalne moduły NanOS) — LinuxKPI to **warstwa source-compat** dla obcego
kodu sterowników.

**Sekwencja:** powierzchnię KPI projektujemy w Q5–Q6 (równolegle do SMP, bo dzieli z nim
prymitywy synchronizacji); pierwszy realny sterownik przez shim (PoC: prosty NIC lub
DRM `i915` dla Latitude 5310) w Q8 / jako duży follow-on po 1.0. **Uwaga licencyjna (GPL):**
sterowniki Linuksa to GPLv2 — trzymamy je jako **osobne moduły** (jak FreeBSD `drm-kmod`),
nie wkompilowujemy w rdzeń NanOS; granica licencyjna = interfejs LinuxKPI.

**Bramki (gates) — twarde zależności:**
- `init.nxe` 64-bit działa **⟹** rebuild portów rusza (Strumień B tail).
- x86_64 bootuje + przerwania **⟹** integracja USB/storage/SMP na realnym jądrze.
- Wszystko zielone w QEMU x86_64 **⟹** pierwsze flashowanie na Latitude 5310 (lab).

### 1.2. Strategia pakietów (`nap` — Nano Package Manager)

`nap` nie jest pustym punktem Q8: ma osobne repo **`~/Projects/nano-packages`** na gałęzi
`feat/nap-mvp`. Stan na 2026-06-15:

- `server/` — Django + DRF + sqlite registry, modele `Package`/`Release`, upload ZIP STORE,
  API `/api/v1/packages/...`, testy serwera.
- `client/` — Rust `no_std`: `napcore` z logiką testowaną na hoście przez trait `Sys`
  (HTTP, JSON, ZIP STORE, sha256, DB installed, install/remove/upgrade/list) oraz cienki
  crate urządzeniowy `nap` z FFI do libc NanOS.
- Kontrakt runtime: pakiety instalują do `/disks/main/...`, link-farm app używa symlinków
  partycyjnych (`/apps/<name>/<name>.nxe`), domyślne repo w QEMU to `http://10.0.2.2:8000`.

**Decyzja strategiczna:** `nap` rozwijamy równolegle jako multi-repo produkt, ale nie
blokujemy nim portu x86_64. Najpierw utrzymujemy go jako host-testowalny klient+serwer
oraz i686 proving ground; po Bramce Q2 dostaje target `x86_64-nanos` i staje się
domyślnym sposobem dokładania portów. To zmienia Q8 z „dopiero zaczynamy pakiety" na
„pakiety są dojrzałe, regresyjnie testowane i używane do dystrybucji portów".

---

## 2. Governance repo (Strumień 0 — fundament, dzień 1)

### 2.1. Stan faktyczny historii

- **Oryginał „zero-AI" jest na `master` (= `origin/master`)**, czubek `b83b433
  "update runtime"`, **2026-02-05** — ostatni ludzki commit przed pracą z agentami.
  Gałąź `develop` **nie istnieje** (mimo potocznej nazwy).
- Praca AI zaczyna się **2026-06-06** (`1d806d8`) na `dockerized-build`, odgałęzionej
  prosto od `b83b433`. Bieżący `feat/pthread` jest **ponad 400 commitów** przed `master`
  (404 w analizie z 2026-06-15, + późniejsze: coreutils itd.).
- **Daty commitów są nieusuwalne** (author-date w obiekcie git); oryginał jest
  bezpieczny na `origin/master`. Chodzi o **trwałe zakotwiczenie i ochronę**.

### 2.2. Nienaruszalne zachowanie oryginału (AKCJA #1, przed jakimkolwiek ruchem `master`)

> ✅ **ZROBIONE (2026-06-16):** tag `original-zero-ai-2026-02-05` + `legacy/original-zero-ai`
> (@ `b83b433`) utworzone i wypchnięte na origin. ⬜ Pozostaje: **branch protection** na origin
> (no force-push / no delete dla tagu + `legacy/*`).

```
git tag -a original-zero-ai-2026-02-05 b83b433 -m "Pristine zero-AI NanOS baseline (last human commit)"
git branch legacy/original-zero-ai b83b433
git push origin original-zero-ai-2026-02-05
git push origin legacy/original-zero-ai
# branch protection na origin: no force-push, no delete (tag + legacy/*)
```
Tag jest nieruchomy i odporny na GC; `legacy/original-zero-ai` jest widoczna i daje się
zworktree'ować do audytu „co napisał człowiek". To spełnia „nie może zaginąć z datami".

### 2.3. Model gałęzi (zatwierdzony: `master`→legacy, nowy `main`+`develop`)

> ✅ **ZROBIONE (2026-06-16):** struktura założona lokalnie i na origin; domyślna gałąź GitHub
> przepięta na `main`; stare scalone gałęzie (`feat/pthread`, `feat/nap-package-manager`,
> `dockerized-build`) usunięte. `master` przemianowany na **`legacy/master`** (lok. + origin).

```
legacy/original-zero-ai   (= tag original-zero-ai-2026-02-05)  ← NIETYKALNE, oryginał   ✅
legacy/master             (= dawny master @ b83b433)           ← wskaźnik historyczny   ✅
main                      ← stabilny trunk; domyślna gałąź origin                       ✅
develop                   ← integracja: agenci mergują tu; CI + headless-QEMU bramkuje  ✅
release/qN                ← (opcjonalnie) snapshot kwartalny pod sprzęt                 ⬜
feat/<agent>/<temat>      ← per-agent, KAŻDY w osobnym worktree                          ⬜
```

Mechanika cut-over (po 2.2): ✅ wykonana —
1. `main` ← obecny dorobek (promowany z `feat/pthread` po zazielenieniu). ✅
2. `develop` ← `main`; codzienna integracja agentów. ✅
3. `master` nie jest już trunkiem — przemianowany na `legacy/master` (kotwica była już na
   origin, więc warunek z 2.2 spełniony). ✅

### 2.4. Worktree governance (równoległa praca 30 agentów)

- Każdy agent: `git worktree add ../nanos-wt/<temat> -b feat/<agent>/<temat> develop`
  → izolowany katalog roboczy; brak kolizji.
- **Pułapka build-infra (zadanie Q1, Strumień 0/G):** `make run` używa współdzielonych
  `disk/image-grub2.img` i monitora `/tmp/qmon` — 30 równoległych QEMU na to nadepnie.
  Sparametryzować per-worktree: `IMAGE=$(pwd)/disk/image.img`, `QMON=/tmp/qmon-$(basename
  $PWD)`, nazwy kontenerów Docker z sufiksem worktree.
- Narzędzie `Agent`/workflow z `isolation: "worktree"` używamy dla zadań mutujących pliki.

### 2.5. Worktree „wysoki" do testów na realnym sprzęcie

Dedykowany, **długożyjący worktree przypięty do `main`** (`../nanos-hw-release`),
odseparowany od churn worktree feature'owych. Z niego buduje się obrazy na Latitude 5310
i flashuje do labu (Strumień G, Q7–Q8). „Co ląduje na laptopie" jest odprzężone od
pracy 30 agentów.

### 2.6. Multi-repo

SDK i pakiety żyją w osobnych repo (`nanos-sdk`, `nanos-sdk-work`, `bash-nanos`,
`nano-packages`, `*-port`). Ta sama dyscyplina: zachowanie baseline +
`main`/`develop`/`feat/*`, bo migracja x86_64 i dystrybucja przez `nap` ich dotyka.

---

## 3. Strumienie (10 strumieni, ~30 osób peak)

| # | Strumień | Osób | Główna odpowiedzialność | Kluczowa zależność |
|---|---|---|---|---|
| **A** | Rdzeń x86_64 | ~5 | boot long-mode, paging 4-poziomowy, przerwania, syscall/sysret, GDT/IDT/TSS 64-bit | **krytyczna ścieżka, nic nie blokuje** |
| **B** | Toolchain + ABI userland + rebuild portów | ~5 | `x86_64-nanos`, `.nxe` v4, `mknx`, `libc.ndl`, crt0/sigtramp, rebuild ~12 portów | ABI syscalli od A; rebuild po `init.nxe` 64-bit |
| **C** | SMP / współbieżność | ~5 | APIC/x2APIC, per-CPU, audyt+wdrożenie blokad jądra, scheduler SMP, dokończenie pthread/musl | jądro x86_64 (A) |
| **D** | Boot & firmware | ~3 | UEFI (x86_64-efi GRUB / własny stub), GOP, ACPI MADT/FADT, GPT, FAT (ESP) | trampolina/boot od A |
| **E** | Magazyn & magistrale | ~4 | PCIe enum, NVMe, AHCI/SATA, virtio-blk/net, partycje | przerwania+paging od A |
| **F** | Stos USB | ~4 | xHCI, USB core, HID (klawiatura/mysz), mass storage (bulk/SCSI) | rdzeń host-testowalny od dnia 1; integracja po A |
| **G** | MM/POSIX/QA + lab sprzętowy | ~4 | COW fork, demand paging, page cache, mmap plików, epoll/poll/timerfd, hardening; CI matrix, lab Latitude 5310, fuzzing | rozproszona; lab po pierwszym boocie x86_64 |
| **H** | Linux Driver Compat (LinuxKPI) | 0→~4 | model urządzeń zgodny z Linuksem, shim API jądra Linuksa, pierwszy sterownik przez rekompilację (GPU `i915` / WiFi `iwlwifi`) | prymitywy SMP (C) + PCIe/DMA (E); ramp-up Q5 |
| **I** | Desktop / NanWM / UX | ~3 | wdrożenie redesignu (glass/blur compositor), window management, apki desktopowe (Files/Settings/Terminal), integracja wejścia/ekranu, jakość daily-driver | userland, równoległy od dnia 1; res. GOP (D), wejście USB/touchpad (F/G) |
| **J** | Pakiety / `nap` | 0→~2 | repo `nano-packages`: Django registry, Rust `napcore`/`nap.nxe`, format paczek ZIP STORE, E2E install/remove/upgrade, katalog portów | sieć+ext RW już działają na i686; x86_64 rebuild po B; bare-metal po E/D |

**Przepływ osób (headcount = peak/orientacyjny, nie steady-state).** Strumienie ramp-up/
ramp-down między kwartałami: **A** (x86_64-specyficzne) kończy się po Q2 → zasila C/I/H;
**D** i **F** wygasają po Q4 → zasilają G/H/I/J. **H** rusza od ~0 (tylko wpływ na decyzje
C/E w Q3–Q4), ramp do ~4–5 w Q5–Q8. **I** działa stale (jakość user-facing to długi ogon).
**J** jest mały, bo większość kodu `nap` jest host-testowalna i już żyje w osobnym repo; jego
koszt rośnie dopiero przy E2E, katalogu pakietów i podpisach/QA.

Strumienie **dojrzałe** w trybie utrzymaniowym: **sieć TCP/IP** i **ext RW** — portują się
przy x86_64/SMP, bez nowych dużych prac w Q1–Q4. (NanWM **nie** jest „utrzymaniowy" — przy
celu daily-driver desktop wymaga aktywnego dopracowania → dedykowany Strumień I.)

---

## 4. Backbone kwartalny (8 kwartałów)

### Q1 — Fundament: x86_64 wstaje
- **A:** trampolina long-mode w `loader.s` (identity-map 2 MiB hugepages, PAE+LME+PG),
  64-bit GDT, skok do 64-bitowego `kmain` (znak na VGA = pierwszy dowód życia). Paging
  4-poziomowy (`AddressSpace`/`mmu` → 64-bit, NX) — rozwijane **TDD na hoście**.
  > 🟡 CZĘŚCIOWO (2026-06-16): trampolina + 64-bit GDT + dowód życia na VGA ✅ ZROBIONE
  > (`plans/2026-06-15-x86_64-plan-1-foundation.md`, „NanOS x86_64 long mode OK" w QEMU).
  > ⬜ Pozostaje paging 4-poziomowy host-TDD (Plan 3).
- **B:** `arch/x86_64/arch.mk`, `x86_64-elf` w Dockerze, `nasm -f elf64`,
  `OUTPUT_FORMAT(elf64-x86-64)`, `qemu-system-x86_64`. Pusty `arch/x86_64` kompiluje się.
  > ✅ ZROBIONE (2026-06-16): Plan 1 — drugi toolchain w Dockerze, `arch/x86_64/{arch.mk,
  > linker.ld,boot/}`, `make ARCH=x86_64 bringup64` bootuje ELF64 przez GRUB ISO (QEMU `-kernel`
  > nie ładuje ELF64). i686 bez regresji.
- **C:** dokończenie pthread/musl na i686 (proving ground); **audyt blokad jądra** pod SMP
  (gdzie są niejawne założenia jednoprocesorowości).
- **D:** projekt ścieżki UEFI; **analiza BIOS Latitude 5310 — czy CSM w ogóle dostępny**
  (Comet Lake 2020 bywa UEFI Class 3). Jeśli brak CSM → bootstrap musi być UEFI od początku.
- **F:** rdzeń USB host-testowalny (deskryptory, maszyny stanów, enumeracja) — bez sprzętu.
- **G:** Strumień 0 governance (tag/legacy/`main`/`develop`); per-worktree build-infra; CI.
- **I:** wdrożenie redesignu UI (glass/blur compositor z `.claude/nanoos-ui/`) na i686;
  window management (snapping, alt-tab, dock/taskbar, spójne dekoracje). Praca userland,
  rebuild pod x86_64 po Bramce Q2.
- **J:** `nano-packages` jako równoległy produkt: domknąć testy serwera i `napcore`,
  utrzymać 100% coverage host-testowalnej logiki, spisać kontrakt integracji z NanOS
  (`NAP_CLIENT`, `/disks/main`, symlink targety, `NAP_REPO`).
- **Bramka Q1:** x86_64 bootuje w QEMU, drukuje, ma paging 4-poziomowy (host-testy zielone);
  redesign NanWM działa w QEMU (i686); `nap` server+client core zielone na hoście.

### Q2 — Fundament: jądro x86_64 kompletne
- **A:** GDT/IDT/TSS 64-bit (TSS 16 B, IST dla #DF/#PF), `isr/irq` z ręcznym zrzutem
  rejestrów + `swapgs` + `iretq`, `syscall`/`sysret` (MSR LSTAR/STAR/FMASK), numery
  syscalli x86_64. Klawiatura/PIT/IRQ działają. **`%fs.base` dla TLS user** (arch_prctl).
- **B:** userland ABI: `crt0.S`/`libnanos`/`sigtramp` na System V AMD64; format `.nxe` v4
  (adresy 64-bit, `R_X86_64_64`/`R_X86_64_32S`, niskie bazy <2 GiB), `mknx`/`NxeLoader`.
  **`init.nxe` 64-bit działa.**
- **E:** `ATA.S` 64-bit (drobne) — odtworzenie boot z ext4 na 64-bit.
- **G:** MI cleanup LP64 (FrameAllocator, SyscallDispatch args→`uintptr_t`, Console hex 64-bit,
  off_t/stat) — sterowane `-Wconversion`.
- **J/B:** dodać target `x86_64-nanos` dla klienta `nap` i regułę Makefile bez uzależniania
  `make image` od zewnętrznego repo; `nap.nxe` jest opcjonalnym artefaktem userland, nie blockerem
  dla bootu kernela.
- **Bramka Q2 (KAMIEŃ MILOWY):** x86_64 bootuje z ext4, `init.nxe`→`nsh` działa, klawiatura
  i shell w QEMU. `arch/x86/` (i686) przechodzi w tryb referencyjny/legacy i może zostać
  usunięty dopiero po Q4 bare-metal, jeśli x86_64 ma pełny parity smoke. Rebuild portów
  odblokowany.

### Q3 — Bring-up sprzętu: firmware + magazyn
- **D:** UEFI realnie (GRUB `x86_64-efi` lub własny stub) + **GOP framebuffer**; ACPI MADT
  (lista CPU dla SMP) + FADT (poweroff/reboot); GPT parsing; FAT (read) dla ESP.
- **E:** PCIe enumeracja (ECAM/MMCONFIG); **NVMe** (M.2 — podstawowy dysk 5310); AHCI/SATA
  tylko jeśli egzemplarz ma dysk SATA (do potwierdzenia); virtio-blk/net (ścieżka QEMU).
  Partycje GPT zamontowane. **Uwaga:** w 5310 priorytet ma NVMe (odwrotnie niż na EliteBooku).
- **B:** rolling rebuild portów: bash → grep/vim (czysta rekompilacja).
- **F:** **xHCI** (rings, ERST, slot/endpoint) + integracja z przerwaniami x86_64.
- **G:** projekt COW fork + demand paging (host-testowalny na `AddressSpace`).
- **J:** pierwsze QEMU E2E `nap install hello`: dev-serwer Django na hoście (`10.0.2.2:8000`),
  pakiet ZIP STORE, zapis do `/disks/main`, `nap list`, reboot persistence. To może działać
  na i686 albo x86_64, ale kontrakt pakietu musi być już arch-aware.
- **Bramka Q3:** boot UEFI+GOP w QEMU (OVMF); dysk przez NVMe/virtio; xHCI wykrywa urządzenia.

### Q4 — Bring-up sprzętu: USB + storage + pierwszy bare-metal
- **F:** USB core + **HID** (klawiatura/mysz USB) + **mass storage** (bulk-only, SCSI read).
- **E:** **NVMe** dopracowane pod M.2 Latitude 5310; NIC: rozszerzenie e1000 → **e1000e/I219**
  — 5310 **ma natywny RJ45** (Ethernet wbudowany); wariant chipa I219-LM vs -V do potwierdzenia.
- **I:** integracja desktopu ze sprzętem: natywna rozdzielczość **GOP**, kursor/mysz przez
  **USB-HID** (od F) lub **I2C-HID touchpad**; apki Files/Settings/Terminal z trybu demo
  do produkcyjnych.
- **D:** FAT write (zapis do ESP); pełny ACPI poweroff/reboot na sprzęcie.
- **B:** rebuild portów sieciowych (inetutils/ping/wget), OpenSSL (NanOS/portable
  x86_64 config, nie ABI Linuksa), Dropbear.
- **G:** **PIERWSZE FLASHOWANIE NA LATITUDE 5310** (UEFI — a jeśli BIOS udostępnia CSM, można
  użyć Multiboot1 jako tańszego bootstrapu; do potwierdzenia w Q1). Lab sprzętowy: rig z
  Latitude 5310, serial/USB-debug, automatyczny flash z worktree `main`.
- **J:** bare-metal smoke `nap list` + instalacja małego pakietu przez LAN, jeśli NIC działa;
  fallback: instalacja z lokalnego ZIP-a na dysku/USB mass storage, żeby testować filesystem i DB
  niezależnie od sieci.
- **Bramka Q4 (KAMIEŃ MILOWY):** NanOS bootuje na **realnym Latitude 5310** (ekran przez GOP,
  klawiatura przez i8042 lub USB-HID, dysk przez NVMe). Shell działa na bare-metal;
  `nap` ma co najmniej jedną ścieżkę instalacji pakietu w labie (LAN albo lokalny ZIP).

### Q5 — SMP: prawdziwa równoległość
- **C:** AP startup (INIT-SIPI-SIPI wg ACPI MADT), per-CPU GS-base, **audyt→wdrożenie
  spinlocków/mutexów** w całym jądrze (scheduler, alokatory, VFS, sieć), **scheduler SMP**
  (per-CPU runqueue + load balancing), TLB shootdown (IPI).
- **C:** pthread/musl z **prawdziwą równoległością** na 4–8 wątkach Latitude 5310.
- **G:** COW fork (zamiast eager copy) + demand paging — duży zysk wydajności fork/exec.
- **J:** `nap remove`/`upgrade` w QEMU + bare-metal rig; format katalogu repo wersjonowany
  tak, żeby porty x86_64 i ewentualne arch-independent assets mogły współistnieć.
- **Bramka Q5:** 2+ rdzenie Latitude 5310 aktywne; równoległy benchmark pokazuje speedup;
  stress-test scheduler/locki stabilny.

### Q6 — Skala + hardening
- **G:** page cache + mmap plików (zamiast pełnego odczytu); opcjonalnie swap.
- **G:** **hardening:** W^X/NX wymuszone (bit NX z 64-bit), KASLR + ASLR, SMEP/SMAP,
  stack canaries, separacja user/kernel audytowana.
- **C/E:** strojenie wydajności pod realny sprzęt (cache, kolejki przerwań MSI/MSI-X).
- **B:** rebuild cięższych portów: NetSurf/libnsfb (audyt arytmetyki offsetów FB), Doom
  (sprawdzić 64-bit-clean), busybox.
- **H:** projekt **modelu urządzeń zgodnego z Linuksem** + powierzchni LinuxKPI (mapowanie
  prymitywów SMP/PCIe/DMA/IRQ z C i E na semantykę jądra Linuksa); szkielet shimu.
- **J:** `nap` staje się oficjalnym kanałem instalacji portów: paczki dla bash/vim/doom/netsurf,
  rollback po błędzie zapisu, sanity-check DB po crash/reboot. HTTPS/podpisy jako projekt Q7/Q8,
  nie warunek MVP.
- **Bramka Q6:** NX/ASLR aktywne i zweryfikowane; page cache mierzalnie przyspiesza I/O;
  pełen ekosystem portów zbudowany 64-bit; `nap install` działa dla realnych portów;
  powierzchnia LinuxKPI zaprojektowana.

### Q7 — Daily-driver: peryferia Latitude 5310
- **G/E:** **Intel HDA audio** (kodek 5310, np. Realtek ALC — do potwierdzenia), **touchpad**
  (precyzyjny **I2C-HID**; PS/2 może nie występować — do potwierdzenia), bateria/ACPI battery
  + lid + thermal (odczyt), podświetlenie/jasność.
- **D:** ACPI sleep (S3) — stretch goal; co najmniej clean shutdown/reboot.
- **G:** POSIX depth: epoll/poll/timerfd/eventfd, `inotify`-lite (dla realnych demonów).
- **I:** finalny polish UX daily-driver: **touchpad** + gesty (od G), Settings sprzętowe
  (jasność/audio/bateria), powiadomienia, schowek, fonty/theming, wydajność software-
  compositora w natywnej rozdzielczości (do czasu akceleracji i915/LinuxKPI w Q8+).
- **J:** UX pakietów: `nap` w Settings/Files/Terminal workflow, cache paczek, czytelne błędy
  offline/ENOSPC/deps, opcjonalnie podpisy repo lub transport HTTPS jeśli OpenSSL jest gotowy.
- **Bramka Q7:** Latitude 5310 jako użyteczny desktop: dopracowany NanWM (touchpad, Settings,
  powiadomienia), dźwięk, sieć, zarządzanie energią (poweroff/reboot/bateria), pakiety
  instalowalne bez ręcznego modyfikowania obrazu.

### Q8 — Dojrzałość: pakiety, self-hosting, stabilność
- **J/G:** menedżer pakietów **`nap`** jako dojrzały kanał dystrybucji: registry w
  `~/Projects/nano-packages`, katalog paczek dla portów, upgrade/remove regresyjnie testowane,
  dokumentacja tworzenia paczek, mirror/dev-server dla labu.
- **B/G:** **próba self-hostingu** — port gcc/binutils na NanOS, kompilacja prostego
  programu on-device (klasyczny kamień dojrzałości; stretch).
- **H:** **shim LinuxKPI + pierwszy sterownik przez rekompilację** — PoC: prosty linuksowy
  NIC lub DRM `i915` (GPU Latitude 5310, Intel UHD) jako osobny moduł GPL. Dowód, że ścieżka
  reuse działa; pełne sterowniki (WiFi `iwlwifi` dla AX201, akceleracja GPU) jako follow-on po 1.0.
- **G:** QA na skalę: CI matrix (QEMU + bare-metal rig), fuzzing syscalli/VFS,
  stress/soak testy, dokumentacja użytkownika.
- **Bramka Q8 (FINAŁ):** NanOS 1.0 „daily-driver" na Latitude 5310 — stabilny, z pakietami,
  udokumentowany, z labem regresyjnym na realnym sprzęcie. W tym samym kwartale domykamy
  bramki `proof`: próba self-hostingu oraz **ścieżka LinuxKPI udowodniona** (≥1 sterownik
  Linuksa działa przez shim). Pełne WiFi `iwlwifi` i akceleracja/modeset `i915` zostają w
  roadmapie jako follow-on po 1.0, nie jako usunięte funkcje.

---

## 5. Macierz sterowników — Dell Latitude 5310 (Comet Lake, 2020)

> Komponenty oznaczone **(do potwierdzenia)** wymagają sprawdzenia na konkretnym egzemplarzu
> (warianty CPU/NIC/storage/BIOS różnią się w obrębie modelu).

| Podsystem | Sprzęt Latitude 5310 | Strumień | Kwartał | Ryzyko / uwaga |
|---|---|---|---|---|
| Boot | UEFI (prawdopodobnie Class 3, **CSM może być niedostępny** — do potwierdzenia) | D | Q3–Q4 | jeśli brak CSM → UEFI wymagane od Q3, brak taniego bootstrapu Multiboot1 |
| Ekran | Intel UHD Graphics (Comet Lake, Gen9.5), **GOP** | D | Q3 | framebuffer GOP od Q3; akceleracja/modeset przez **DRM `i915`/LinuxKPI** (H, Q8+) |
| Klawiatura | i8042 (PS/2, EC) — do potwierdzenia | (jest) / F | Q4 | **NanOS już ma PS/2** → klawiatura „za darmo" jeśli i8042; USB-HID/I2C-HID jako backup |
| Touchpad/pointstick | precyzyjny **I2C-HID** (do potwierdzenia) | G | Q7 | I2C-HID trudniejsze niż PS/2; 5310 prawdopodobnie nie ma PS/2 touchpada |
| Magazyn | **NVMe (M.2 PCIe)** — podstawowy; AHCI/SATA tylko jeśli wariant ma (do potwierdzenia) | E | Q3–Q4 | **NVMe priorytetem** (odwrotnie niż EliteBook); virtio-blk w QEMU |
| Sieć LAN | **natywny RJ45**, Intel **I219-LM/V** (wariant chipa do potwierdzenia) | E | Q4 | rozszerzenie e1000 → e1000e/I219; LAN dostępny od razu (`nap` przez sieć działa) |
| WiFi | **Intel Wi-Fi 6 AX201** (CNVi) — do potwierdzenia | H | Q8+ | od zera poza zakresem; realne przez **`iwlwifi`/LinuxKPI** jako follow-on po 1.0 |
| USB | Intel **xHCI** (USB 3.x, USB-C) | F | Q3–Q4 | mandatory: storage do flashowania, mysz; USB-C może mieć Thunderbolt (do potwierdzenia) |
| Audio | Intel HDA (kodek Realtek ALC — do potwierdzenia) | G | Q7 | stretch; klasyczny HDA |
| Zasilanie | ACPI (bateria/lid/thermal/S3) | D/G | Q7 | poweroff/reboot pewne; S3 stretch |
| CPU | do 4 rdzeni / 8 wątków (i5-10310U; i3 = 2C/4T — do potwierdzenia) | C | Q5 | SMP daje realny speedup |

---

## 6. Zależności krytyczne (ścieżka)

```
Strumień 0 (governance)  ──┐ (dzień 1, nieblokujące)
                           │
A: x86_64 boot ─→ paging ─→ przerwania ─→ syscall ──┬─→ [Bramka Q2: init.nxe 64-bit]
                                                    │
B: toolchain ─────────────→ ABI/.nxe v4 ───────────┘ ─→ rebuild portów (długi ogon, równoległy)
                                                    │
                          (po Bramce Q2 / jądro 64-bit gotowe)
                                                    │
                        ┌───────────────────────────┼───────────────────────────┐
                        ↓                            ↓                            ↓
        D: UEFI/GOP/ACPI            E: PCIe/NVMe/AHCI/virtio        F: xHCI/HID/storage
                        └───────────────┬───────────┴───────────────┬───────────┘
                                        ↓                           ↓
                          [Bramka Q4: bare-metal Latitude 5310]  C: SMP (po jądrze 64-bit)
                                        ↓                           ↓
                                  G: MM/hardening/peryferia/QA  ─→  [Bramka Q8: daily-driver 1.0]
                                                    │
                                                    └─→ J: nap registry/client ─→ paczki portów ─→ upgrade/remove QA
```

Komponenty **host-testowalne / MI** (rdzeń USB, COW na `AddressSpace`, audyt blokad,
dokończenie pthread, cleanup LP64, `napcore`/registry) ruszają **od dnia 1** równolegle
do krytycznej ścieżki A.

---

## 7. Główne ryzyka

| Ryzyko | Wpływ | Mitygacja |
|---|---|---|
| Krytyczna ścieżka A wąska (5 os.) blokuje resztę | wysoki | maks. praca MI/host-testowalna przed Bramką Q2; A nie czeka na nic |
| Model kodu `.nxe` / relokacje 64-bit (jedyna niemechaniczna decyzja) | wysoki | niskie bazy <2 GiB + model small + `R_X86_64_64`/`R_X86_64_32S` (wg analizy x64 §5.4 #1) |
| `%fs.base`/TLS + asymetria SSE (kernel `-mno-sse`, user wymaga SSE) | średni | wzorce znane; testy wcześnie; flagi w `arch.mk` (x64 §5.4 #2,#3) |
| Red zone / `swapgs` / adresy kanoniczne w ścieżce przerwań | średni | review asm, IST dla #DF, testy fault na sprzęcie |
| SMP locking — niejawne założenia jednoprocesorowości | wysoki | audyt Q1 przed wdrożeniem; stress-testy; Big Kernel Lock jako etap pośredni |
| **Brak CSM na Latitude 5310** (Comet Lake/2020) — odpada tani bootstrap Multiboot1 | wysoki | potwierdzić w BIOS w Q1; jeśli brak → UEFI Class 3 wymagane od Q3 (Strumień D priorytet) |
| Dokładny wariant NIC (I219-LM vs -V) nieznany | niski | RJ45 potwierdzony; e1000e/I219 pokrywa oba warianty — `nap` przez LAN działa; fallback lokalny ZIP/USB pozostaje |
| Realny sprzęt ≠ QEMU (warianty firmware/peryferiów) | wysoki | lab z Latitude 5310 od Q4; serial debug; konkretny egzemplarz referencyjny |
| 30 agentów × współdzielona build-infra | średni | per-worktree obraz/socket/kontener (Strumień 0, Q1) |
| Rebuild ~12 portów (dominujący koszt kalendarzowy) | średni | równoległy, odseparowany od kernela; rusza po Bramce Q2; ryzyko rozproszone |
| **`nap` multi-repo drift** (`nano-packages` vs NanOS ABI/ścieżki/syscalle) | średni | jawny kontrakt `NAP_CLIENT`, QEMU E2E od Q3, osobna bramka x86_64 targetu po Q2, testy hostowe `napcore`/server |
| **LinuxKPI ogromny zakres** (FreeBSD budował latami) | wysoki | model urządzeń „Linux-shaped" od Q3–Q4 → cienki shim; tylko PoC w zakresie 1.0, reszta follow-on |
| **Licencja GPL** sterowników Linuksa | średni | LinuxKPI'd sterowniki jako osobne moduły GPL (wzorzec `drm-kmod`); granica = interfejs shimu |

---

## 8. Weryfikacja (per CLAUDE.md + lab)

- **Host:** `make test` (doctest, ≥90% lcov) — rdzeń USB, COW, paging 4-poziomowy,
  `NxeLoader` v4 rozwijane TDD bez sprzętu.
- **Nap host:** w `~/Projects/nano-packages`: server coverage dla Django registry oraz
  `cargo llvm-cov` dla `napcore`; urządzeniowy crate `nap` weryfikowany przez QEMU E2E.
- **QEMU:** `qemu-system-x86_64` headless screendump + `-d int` fault grep; OVMF dla UEFI.
- **Bare-metal:** lab z Latitude 5310 (Strumień G, od Q4) — automatyczny flash z worktree
  `main`, serial/USB-debug, testy regresyjne na realnym sprzęcie przed każdą promocją do `main`.
- **CI:** `develop` bramkowane host+QEMU; `main` bramkowane dodatkowo bare-metal rig.

---

## 9. Pierwsze akcje (sprint 0)

1. ✅ **Governance (AKCJA #1):** tag `original-zero-ai-2026-02-05` + `legacy/original-zero-ai`
   + push **zrobione (2026-06-16)**. ⬜ Pozostaje: branch protection na origin (sekcja 2.2).
2. ✅ **Zrobione (2026-06-16):** `main` (z zazielenionego `feat/pthread`) i `develop` utworzone
   i wypchnięte; `master`→`legacy/master`; stare scalone gałęzie usunięte; domyślna gałąź = `main`.
   ⬜ Pozostaje: spisać konwencję worktree.
3. ⬜ Per-worktree build-infra (obraz/socket/kontener parametryzowane) — odblokowuje 30 agentów.
4. ✅ ZROBIONE (2026-06-16): `arch/x86_64/arch.mk` + `x86_64-elf` w Dockerze — i więcej:
   pełny Plan 1 (`plans/2026-06-15-x86_64-plan-1-foundation.md`) bootuje 64-bitowy `kernel64.bin`
   w QEMU („NanOS x86_64 long mode OK"). Gałąź `feat/x86_64-foundation`.
5. Skonsolidować literały okien VA do kontraktu `arch/mmu.h` (przygotowanie pod 64-bit).
6. **Potwierdzić sprzęt egzemplarza Latitude 5310:** CSM tak/nie w BIOS, CPU (rdzenie/wątki),
   NVMe vs SATA, wariant NIC (I219-LM vs -V; RJ45 jest), WiFi (AX201?), touchpad (I2C-HID?),
   kodek audio — uzupełnić §5.
7. `nano-packages`: utrwalić baseline gałęzi `feat/nap-mvp`, spisać kontrakt z NanOS
   (`NAP_CLIENT`, ścieżki `/disks/main`, default `NAP_REPO`), dodać plan integracji
   x86_64 targetu po Bramce Q2.
8. ✅ ZROBIONE (2026-06-16): cały backbone x86_64 (nie tylko Q1) rozpisany na pełne plany TDD
   bite-sized — `plans/2026-06-15-x86_64-plan-{2..8}-*.md` (72 taski / 327 kroków): Plan 2 boot+konsola+MI
   kmain, 3 paging 4-poziomowy, 4 GDT/IDT/TSS+przerwania, 5 storage, 6 syscalle+userland+.nxe v4,
   7 sprzątanie MI LP64, 8 SDK `x86_64-nanos`+porty. Plan 1 (fundament) już wykonany.
