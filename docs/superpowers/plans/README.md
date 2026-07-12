# Indeks planów NanOS

Katalog wszystkich planów wykonawczych z `docs/superpowers/plans/`, pogrupowany tematycznie, ze
stanem na **2026-07-04**. Projekty projektowe (specs) żyją obok w `docs/superpowers/specs/` — plan
zwykle linkuje swój spec. Priorytety i diagnoza długu: `2026-07-04-tech-debt-priorities.md`
(meta-plan). Uwaga: checkboxy w starych planach bywają nieodhaczone mimo wykonania — kolumna
„Stan" poniżej jest wiążąca.

Legenda: ✅ wykonany · 🔶 wykonany częściowo (wisi ogon) · 🚧 w trakcie · ⬜ niezaczęty · 📓 log/meta

## Fundamenty x86_64 (migracja ZASTĘPUJĄCA i686)

| Plan | Co | Stan |
|---|---|---|
| [x86_64 1: foundation](2026-06-15-x86_64-plan-1-foundation.md) | toolchain, arch-split, LP64 | ✅ |
| [x86_64 2: boot/console/kmain](2026-06-15-x86_64-plan-2-boot-console-kmain.md) | long-mode boot | ✅ |
| [x86_64 3: paging](2026-06-15-x86_64-plan-3-paging.md) | 4-poziomowy PML4; carry-forwardy domknięte (sufit ~1GiB VA, privatyzacja okien) | ✅ |
| [x86_64 4: interrupts](2026-06-15-x86_64-plan-4-interrupts.md) | IDT/IRQ 64-bit | ✅ |
| [x86_64 5: storage](2026-06-15-x86_64-plan-5-storage.md) | ATA/ext na x64 | ✅ |
| [x86_64 6: syscalls/userland](2026-06-15-x86_64-plan-6-syscalls-userland.md) | ABI syscalli, user 64-bit | ✅ |
| [x86_64 7: MI/LP64 cleanup](2026-06-15-x86_64-plan-7-mi-lp64-cleanup.md) | sprzątanie typów | ✅ |
| [x86_64 8: SDK/app ports](2026-06-15-x86_64-plan-8-sdk-app-ports.md) | porty na x64 | ✅ |
| [x86_64 9: unstage](2026-06-15-x86_64-plan-9-unstage.md) | zdjęcie stagingu | ✅ |
| [x86_64 10: userland libc+porty](2026-06-18-x86_64-plan-10-userland-libc-and-ports.md) | domknięcie userlandu | ✅ |

Kasacja martwego drzewa `arch/x86` (i686): zaplanowana w meta-planie długu (S3), bez osobnego planu.

## Współbieżność

| Plan | Co | Stan |
|---|---|---|
| [pthread](2026-06-15-pthread.md) | musl pthread na picolibc, 23 zadania | ✅ |
| [SMP/multicore](2026-06-23-smp-multicore.md) | 4 rdzenie, IPI TLB shootdown | ✅ |
| [BKL retirement audit](2026-06-24-bkl-retirement-audit.md) | fine-grained locki, tortury wyścigów | ✅ |

## Sprzęt / realny hardware (Dell Latitude 5310)

| Plan | Co | Stan |
|---|---|---|
| [Dual-boot UEFI/BIOS](2026-06-19-dual-boot-uefi-bios.md) | Limine na hybrydowym GPT; NanOS bootuje na Dellu | ✅ |
| [Real-HW boot](2026-06-20-real-hw-boot.md) | 5 fixów real-HW (FAT32 ESP, 64-bit MMIO, …) | ✅ |
| [USB stack + live-USB](2026-06-19-usb-stack-live-usb.md) | Stream F: xHCI, MSC rw-root, HID | ✅ |
| [NIC faza 1: core+MSI+e1000e](2026-06-20-nic-phase1-core-msi-e1000e.md) | E1000Core, LAPIC, MSI | ✅ |
| [NIC faza 2: I219](2026-06-20-nic-phase2-i219-ich9lan.md) | kext ich9lan | 🔶 kod gotowy, test na Dellu wisi |
| [Privatyzacja okien VA usera](2026-06-20-user-va-window-privatization.md) | per-proces okna user | ✅ |

Brak sterownika NVMe = bloker daily-drivera; ujęty w meta-planie długu (D1), plan wykonawczy do napisania.

## Sieć / krypto

| Plan | Co | Stan |
|---|---|---|
| [Networking](2026-06-12-networking.md) | pełny TCP/IP, prawdziwy ping | ✅ |
| [Net dociągnięcia](2026-06-12-net-dociagniecia.md) | poprawki stosu | ✅ |
| [TLS/SSL/SSH](2026-06-13-tls-ssl-ssh-openssl.md) | CSPRNG, OpenSSL 3.0.15, Dropbear | ✅ (ogon: teardown sesji dropbeara) |

## Userland / porty

| Plan | Co | Stan |
|---|---|---|
| [Coreutils](2026-06-16-coreutils.md) | sbase in-tree + fixy kernel/libc | ✅ |
| [Git follow-up](2026-06-16-git-port-followup.md) | git 2.54 commituje na NanOS | 🔶 `git gc` + `git -C` do naprawy |
| [htop](2026-06-22-htop-port.md) | htop 3.5.1, /proc rozbudowane | ✅ |
| [Uprawnienia linuksowe](2026-06-22-linux-user-permissions.md) | Cred/DAC/setuid + libc | ✅ (resztę domknęły VT/nwlogin/sudo) |
| [NetSurf](2026-06-13-netsurf-nanos-port.md) | graficzna przeglądarka, osobne repo | ✅ |
| [Natywny toolchain GCC](2026-06-28-native-gcc-toolchain.md) | self-hosting binutils+mknx (fazy 0-2) | ⬜ |
| [Electron apps + MarkText](2026-07-07-electron-marktext/README.md) | reusable Electron runtime/platform; MarkText jako acceptance app | ⬜ |

## GUI / desktop

| Plan | Co | Stan |
|---|---|---|
| [GUI performance](2026-06-11-gui-performance.md) | -O2 userland, frame-cache | ✅ (kernel -O2 → osobny plan niżej) |
| [Backdrop cache blur](2026-06-22-backdrop-cache-blur.md) | CPU glass blur z cache | ✅ |
| [Notepad](2026-06-22-nwnote-notepad.md) | Notepad + rozrost libnwui | ✅ |
| [Wirtualne terminale](2026-06-24-virtual-terminals.md) | Ctrl+Alt+Fn, fbcon, nwlogin greeter | ✅ |
| [Fonty proporcjonalne](2026-06-27-proportional-fonts.md) | stb_truetype, Plex Sans / JetBrains Mono | ✅ |
| [Rust Files (rsexp)](2026-06-27-rust-icon-explorer.md) | eksplorator w Ruście, iconview w libnwui | ✅ |
| [Liquid-glass frames v2](2026-07-03-aero-liquid-glass-frames.md) | szklane ramki whole-window-slab | ⬜ (odblokowany: kompozytor czysty) |

## GPU

| Plan | Co | Stan |
|---|---|---|
| [LinuxKPI + virtio-gpu](2026-06-28-linuxkpi-virtio-gpu.md) | niemodyfikowany sterownik Linuksa 6.12 przez shim | ✅ |
| [GL 1: desktop virgl/QEMU](2026-07-01-plan-1-gl-desktop-virgl-qemu.md) | DRM nodes, Mesa, GBM/EGL, nwm-GL z GPU-blur | ✅ |
| [GL 2: Vulkan venus](2026-07-01-plan-2-vulkan-venus.md) | venus + grunt pod ANV | ⬜ (za bramką GO/NO-GO) |
| [GL 3: i915/Iris na Dellu](2026-07-01-plan-3-gl-on-dell-i915-iris.md) | KPI-upgrade → i915 KMS → execbuf → iris | 🚧 faza A (IRQ/wq/fw ✅, kampania i915 0/276) |
| [Dell GPU test log](2026-07-01-dell-gpu-test-log.md) | dziennik sesji na Dellu | 📓 |
| [GL userspace refactor](2026-07-04-gl-userspace-refactor.md) | źródła-prawdy, settings na GL, FB-cache, tooling | ⬜ |
| [Ujednolicenie nwm](2026-07-04-nwm-unification.md) | JEDEN nwm (runtime-probe sterownika GL), jeden obraz, koniec image64-gl | ⬜ |

## Build / odtwarzalność / dług

| Plan | Co | Stan |
|---|---|---|
| [Reorg repo: manifest+bootstrap](2026-07-02-plan-1-repo-reorg-manifest-bootstrap.md) | ratunek przepisów portów z sdk-work, `nanos-fetch` | 🔁 **SUPERSEDED** przez migrację NanOS-labs (niżej) — recipe-home = per-upstream forki, nie `nanos-sdk/ports/` |
| [Migracja ekosystemu → NanOS-labs](2026-07-12-ecosystem-forks-migration.md) | org NanOS-labs, ~27 repo (forki z przepisami w środku), `ports.manifest`+`bootstrap.sh`+`migrate-fork.sh`, `docs/ECOSYSTEM.md`, brama clean-room | 🟨 w toku (12/14; brama clean-room + push qemu-nanos) |
| [`make world` + BUILDING.md](2026-07-02-plan-2-make-world-building-docs.md) | świeży Linux buduje wszystko; serwer/CI | ⬜ |
| [Host: macOS od zera + Ubuntu](2026-07-04-host-reproducibility-macos-ubuntu.md) | setup hosta, stack GL na obu OS, KVM, bramy clean-machine | ⬜ |
| [ext path-scan UB / kernel -O2](2026-07-04-ext-path-ub-kernel-o2.md) | fix OOB w lookupach ext → kernel wraca na -O2 | ⬜ **(K2)** |
| [Priorytety długu technicznego](2026-07-04-tech-debt-priorities.md) | meta-plan: diagnoza K/D/S + kolejność | 📓 meta |

## Rekomendowana kolejność niezaczętych (spójna z meta-planem długu)

1. **Reorg repo (K1)** → **`make world` (Plan 2)** → **GL refactor (T1-2 mogą iść równolegle z Planem 2)** → **Ujednolicenie nwm** (po T1 refactoru; Task 1 sondy można zacząć od razu) → **Host macOS/Ubuntu** — jeden łańcuch odtwarzalności; po nim żadna wiedza nie żyje tylko na tym Macu.
2. **ext-UB/kernel -O2 (K2)** — niezależny, duży zysk perf + bug poprawności.
3. **i915 (w toku, osobna sesja)** — nie wchodzić jej w drogę; po merge'u dołożyć wyciszenie telemetrii lkpi i page-flip.
4. Potem wg uznania: liquid-glass v2, Vulkan (po bramce), natywny GCC, NVMe (plan do napisania).

---

*Aktualizacja indeksu: przy dodaniu/ukończeniu planu popraw wiersz tutaj (jedna linia), nie rozbudowuj opisów — szczegóły żyją w planach.*
