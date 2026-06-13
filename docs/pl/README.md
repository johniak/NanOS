# Dokumentacja NanOS (polski)

Dokumentacja architektury jądra i userlandu NanOS. Wersja angielska: [`../en/`](../en/README.md).

| Dokument | Co opisuje |
|---|---|
| [boot.md](boot.md) | Sekwencja startowa: GRUB2/Multiboot → `loader.s` → `kmain` → `Kernel::start`, GDT/IDT/PIC, fix potrójnego faultu, przekazanie do `init`. |
| [memory.md](memory.md) | Frame allocator, paging, per-proces przestrzenie adresowe, kernel heap, pełna mapa pamięci, `brk`/`mmap`. |
| [scheduler.md](scheduler.md) | Scheduler z deferred-preemption, task vs proces, context switch, blokowanie/wybudzanie, fork/exec/wait, sygnały. |
| [syscalls.md](syscalls.md) | ABI `int 0x80` (Linux i386), rdzeń `Syscalls`, pełna tablica dispatchu, errno, stuby. |
| [filesystem.md](filesystem.md) | Przestrzeń nazw VFS, układ na dysku, ext2/ext4 read+write + JBD2, linki, stos storage. |
| [nxe-ndl.md](nxe-ndl.md) | Format `.nxe`/`.ndl`, loadery (mknx/NxeLoader/DynLoader), linkowanie dynamiczne, cykl życia procesu w ring 3. |
| [kext.md](kext.md) | Ładowalne moduły jądra (`.nkext`): format, loader, ABI kernel↔kext, istniejące sterowniki. |
| [pci.md](pci.md) | Magistrala PCI: enumeracja, dekodowanie BAR, backend config-space MI/MD, hook wykrywania sterowników (e1000). |
| [terminal.md](terminal.md) | Konsola (VGA→fbcon), klawiatura evdev, PTY + line discipline + job control, współdzielony silnik VT, `nterm`. |
| [ipc-time.md](ipc-time.md) | Potoki (ring FIFO `pipe(2)`) i podsystem czasu (epoka bootu z RTC + tick monotoniczny, `clock_gettime`, uptime). |
| [lib.md](lib.md) | Warstwa narzędziowa freestanding: `<string.h>`, kontenery `String`/`List` i glue C++ ABI. |
| [networking.md](networking.md) | Pełny stos IPv4: e1000 → Ethernet/ARP → IP → ICMP/UDP/RAW/TCP → gniazda BSD → ABI gniazd Linuksa, DHCP, `/proc/net`. |
| [windowing.md](windowing.md) | NanWM: kompozytor `nwm`, protokół klient↔kompozytor, `libnw`, toolkit `libnwui`, aplikacje GUI. |
| [writing-apps.md](writing-apps.md) | How-to: pisanie aplikacji wbudowanej, aplikacji GUI (NanWM) i portowanie realnych programów linuksowych z nanos-sdk. |

Specyfikacje i plany projektowe znajdują się w [`../superpowers/`](../superpowers/).
