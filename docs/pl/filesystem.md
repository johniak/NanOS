# Układ systemu plików NanOS

Słowo "filesystem" odnosi się tutaj do dwóch odrębnych rzeczy i warto je rozróżniać:

1. **Przestrzeń nazw VFS** — ujednolicone drzewo ścieżek widziane przez działający program
   (`/`, `/disks`, `/dev`, `/proc`, `/etc`, `/tmp`, …). Jest składane przy starcie z kilku
   sterowników filesystemów zamontowanych w różnych punktach. Nic nie jest "zamontowane w `/`"
   w uniksowym sensie dysku — root jest syntetyczny.
2. **Układ na dysku** — drzewo katalogów faktycznie zapisane w obrazie dysku ext2/ext4,
   który montowany jest w przestrzeni nazw pod `/disks/main`.

---

## 1. Przestrzeń nazw VFS (runtime)

Składana w `kernel/Kernel.cpp` (`Kernel::start`). Cały dostęp przechodzi przez VFS
(`fs/Vfs.*`), który kieruje ścieżkę do właściwego sterownika według dopasowania mountpointa
o najdłuższym prefiksie.

```
/                     SynthFs  — synthetic in-memory root (no disk). Holds the mount
│                              points + /proc + /dev. Read-only, generated on the fly.
├── disks/
│   └── main/         ext2/ext4 (auto-detected) — THE physical disk image, partition
│                     discovered from the MBR. READ-WRITE: writes persist to disk and
│                     survive reboot (see §2 + §5). Everything persistent lives here.
├── dev/
│   ├── fb0           CharDevice — the firmware framebuffer (fbdev ioctls + mmap +
│   │                 read/write). Present only if the bootloader provided a framebuffer.
│   ├── input0        CharDevice — the keyboard as a Linux-style evdev: the PS/2 IRQ feeds
│   │                 scancodes, programs read() 2-byte key down/up events.
│   ├── ptmx         CharDevice — PTY master, held by the userspace terminal emulator.
│   ├── pts0         CharDevice — PTY slave, the shell's controlling tty.
│   └── tty          CharDevice — the controlling terminal (same slave as pts0 with one PTY).
├── proc/             SynthFs — synthetic, Linux-style. Generated per read:
│   ├── meminfo       MemTotal/MemFree (physical RAM) + KHeapTotal/KHeapFree (kernel heap).
│   ├── uptime        seconds since boot (advances via the scheduler clock).
│   ├── version       kernel identification string.
│   └── <pid>/…       one entry per process + kernel thread (e.g. [idle]).
├── etc/              RamFs — a writable in-memory tmpfs, populated at boot from the on-disk
│                     template /disks/main/nanos/config/etc/. Linux apps find resolv.conf,
│                     hosts, nsswitch.conf, protocols, services here (DHCP rewrites
│                     resolv.conf at runtime). Lives in tmpfs so runtime edits don't touch
│                     the disk image; cleared/regenerated on reboot.
└── tmp/              RamFs — a writable in-memory tmpfs. Cleared on reboot. Programs put
                      transient files here (e.g. Doom's config + savegames).
```

**Dlaczego syntetyczny root zamiast montowania dysku w `/`?** Utrzymuje to widoczną dla maszyny
przestrzeń nazw (urządzenia, informacje o procesach, miejsce na pliki tymczasowe) niezależną od
jakiegokolwiek pojedynczego dysku oraz pozwala montować wiele wolumenów obok siebie pod
`/disks/<name>` — w stylu macOS/Plan 9, a nie w stylu uniksowym "jeden dysk jest rootem". Dysk
to tylko jeden z mieszkańców pod `/disks`.

**Możliwość zapisu:** sterownik ext (`fs/ExtFilesystem.*`) jest teraz **w pełni do odczytu i
zapisu** — zapisy do `/disks/main` alokują bloki/inody, aktualizują metadane i **utrzymują się na
dysku po restarcie**, z księgowaniem (journalling) przez JBD2 (patrz §5). Pamięciowe `/tmp` i
`/etc` (RamFs) również są zapisywalne, ale ulotne (czyszczone przy restarcie). Tylko `/` i `/proc`
są tylko do odczytu — są generowane w locie, nigdzie nieprzechowywane.

---

## 2. Układ na dysku (`/disks/main`)

Obraz ext2/ext4 (`disk/image-grub2.img`, budowany przez `scripts/create-grub2-image.sh`,
wypełniany przez cel `_image` w Makefile). Ułożony tak, aby **system operacyjny, aplikacje
użytkownika i PATH do uruchamiania po nazwie były czysto rozdzielone**:

```
/disks/main/
├── boot/grub/                 GRUB2 stages + grub.cfg (the bootloader; not touched at runtime).
│
├── nanos/                     EVERYTHING that IS the operating system lives here.
│   ├── core/
│   │   ├── kernel.bin         the kernel (loaded by GRUB via multiboot).
│   │   └── init.nxe           PID 1 — the first user program; execve()s into the shell.
│   ├── bin/                   SYSTEM utilities (flat, no bundle): nsh, ls, cat, free.
│   ├── lib/                   SHARED LIBRARIES (.ndl): libc.ndl, greet.ndl. The dynamic
│   │                          loader (kernel/DynLoader.cpp) resolves "needed" libraries
│   │                          here, by name, at exec time.
│   ├── kext/                  loadable kernel modules (.nkext): PS/2 keyboard + mouse, e1000
│   │                          NIC. Loaded at boot by loadAllKexts() — NOT in the kernel image.
│   │                          See kext.md.
│   ├── config/                system config (NanOS's /etc). Holds `passwd` — the account
│   │                          database; its 7th field is the login shell, so editing it sets
│   │                          the default shell (init/nterm launch getpwuid()->pw_shell).
│   │                          `config/etc/` is the template copied into the writable /etc
│   │                          tmpfs at boot (resolv.conf, hosts, nsswitch.conf, …).
│   ├── cache/ logs/           reserved: caches / logs (future).
│
├── apps/                      NON-SYSTEM apps, each a self-contained BUNDLE directory:
│   └── <name>/
│       ├── <name>.nxe         the app's entry binary.
│       └── …                  the app's own data files (e.g. apps/doom/doom1.wad).
│
└── bin/                       LINK FARM: a flat directory of symlinks
    └── <name>.nxe  ->  /apps/<name>/<name>.nxe   (a symlink, NOT a copy)
```

### Dlaczego taki podział

- **`/nanos` = system operacyjny, jedno samodzielne poddrzewo.** Rdzeń (kernel + init),
  narzędzia systemowe, biblioteki współdzielone i przyszłe dane systemowe — wszystko mieszka pod
  jednym katalogiem, więc "system operacyjny" jest pojedynczą, przenośną/identyfikowalną rzeczą
  (w stylu NeXT/macOS `/System`).
- **`/nanos/bin` (system) vs `/apps` (cała reszta).** `ls`/`cat`/`free`/`nsh` są częścią systemu
  i leżą płasko w `bin`. Gra albo demo (`doom`, `usedll`) lub narzędzie testowe **nie** jest
  systemem — mieszka we własnym katalogu-bundle `apps/<name>/` razem ze swoimi danymi. Usunięcie
  lub dodanie aplikacji to po prostu dodanie/usunięcie jednego katalogu.
- **Bundle aplikacji** trzymają program i jego zasoby razem (na wzór macOS `.app` / folderu gry).
  `doom` otwiera swój IWAD po ścieżce absolutnej `/disks/main/apps/doom/doom1.wad` — binarka i dane
  dostarczane są jako jedna całość.
- **`/bin` jest link farmą** — sposób, w jaki shell uruchamia aplikacje *po nazwie* bez znajomości
  układu bundli (wzorzec `/usr/local/bin` → Homebrew Cellar). Każdy wpis jest **dowiązaniem
  symbolicznym** do bundle, więc nie ma drugiej kopii binarki; usuń bundle, a dowiązanie zawiśnie.

### Jak rozwiązywane jest polecenie (`user/nsh.c`)

Dla gołej nazwy polecenia shell próbuje, w kolejności:

1. `/disks/main/nanos/bin/<cmd>.nxe`   — narzędzie systemowe,
2. `/disks/main/bin/<cmd>.nxe`         — aplikacja przez link farmę (symlink → bundle),
3. `/disks/main/apps/<cmd>/<cmd>.nxe`  — bundle bezpośrednio (fallback).

Nazwa zaczynająca się od `/` jest uruchamiana jako jawna ścieżka. Biblioteki współdzielone, których
plik wykonywalny `needs`, są zawsze rozwiązywane z `/disks/main/nanos/lib/<name>.ndl`, niezależnie
od tego, gdzie sam program się znajduje.

---

## 3. Dowiązania

Sterownik ext (`fs/ExtFilesystem.h`) rozwiązuje oba rodzaje dowiązań podczas wyszukiwania ścieżki,
a także potrafi je **tworzyć** (`symlink()`, `link()`):

- **Dowiązania symboliczne** — `getInodeByPath` podąża za komponentem-symlinkiem do jego (absolutnego)
  celu, ze strażnikiem głębokości chroniącym przed pętlami. Krótkie cele czytane są inline z
  60-bajtowego obszaru `i_block` inoda (ext "fast symlink"); długie z bloku danych ("slow
  symlink"). Wykorzystywane przy link farmie `/bin`.
- **Dowiązania twarde** — działają przezroczyście: dowiązanie twarde to po prostu drugi wpis
  katalogowy wskazujący na ten sam numer inoda, więc rozwiązywanie trafia na ten sam plik bez
  żadnego specjalnego kodu. (Zweryfikowane w `tests/test_ext2.cpp`.)

---

## 4. Stos pamięci masowej (jak odczyt/zapis ścieżki dociera na dysk)

```
VFS (fs/Vfs)              path routing: longest-prefix mountpoint -> a FileSystem driver
  ├── SynthFs             generated trees: / and /proc and /dev nodes
  ├── RamFs               in-memory read/write tmpfs (/tmp, /etc)
  └── ExtFilesystem       ext2/ext4 read+write core (superblock, inodes, dirs, symlinks)
        │                   helpers under fs/ext/:
        │                     BlockCache    write-back block cache (dirty tracking + flush)
        │                     ExtAllocator  block/inode bitmap alloc/free (+ csums)
        │                     Journal       JBD2 transaction log (replay/write/reset)
        ├── Ext2Filesystem  resolveBlock = direct (+ indirect) blocks
        └── Ext4Filesystem  resolveBlock = extent tree

BlockDevice (drivers/BlockDevice.h)   HAL: readSectors/writeSectors/sectorSize
  ├── AtaBlockDevice     real disk (ATA PIO), one command per sector (read AND write)
  └── RamBlockDevice     in-memory buffer (and the test fixtures)

DeviceManager            runtime registry of block devices
```

**Odczyt** `/disks/main/apps/doom/doom.nxe` → VFS dopasowuje mount `/disks/main` → sterownik
ext rozwiązuje ścieżkę (podążając za symlinkiem, jeśli przyszedł przez `/bin`) →
`resolveBlock` mapuje przesunięcia w pliku na bloki dysku → `AtaBlockDevice` wykonuje odczyty ATA.

**Zapis** do `/disks/main/...` → sterownik ext alokuje bloki/inody przez `ExtAllocator`,
aktualizuje bloki inoda/katalogu/bitmapy przez `BlockCache`, i przy każdej operacji wywołuje
`txFlush()`, który księguje brudne metadane jako transakcję JBD2 (log → checkpoint →
reset), zanim pozwoli `AtaBlockDevice` zapisać je na fizyczny dysk (patrz §5).

---

## 5. Obsługa zapisu i księgowanie

Sterownik ext jest pełnym filesystemem do odczytu i zapisu zarówno dla ext2, jak i ext4:

- **Dane plików:** `write()` (z alokacją bloków), `truncate()` (powiększanie/zmniejszanie,
  zwalnianie lub alokacja bloków), `create()` (utworzenie-lub-obcięcie zwykłego pliku).
- **Katalogi:** `mkdir()`, `rmdir()`, `unlink()`, `rename()`, oraz `link()` / `symlink()`.
- **Metadane:** `chmod()`, `chown()` / `lchown()`, `utimes()`.
- **Alokacja** (`fs/ext/ExtAllocator.*`): bitmapy bloków i inodów per grupa, liczniki wolnych
  zasobów utrzymywane na bieżąco w superbloku, oraz przeliczane sumy kontrolne bitmap /
  deskryptorów grup dla ext4 `metadata_csum`.
- **Cache bloków** (`fs/ext/BlockCache.*`): write-back cache, który przygotowuje brudne bloki
  (`write` / `writePartial` dla read-modify-write) i sbrasowuje je na urządzenie; `Journal`
  steruje selektywnym flushem (najpierw bloki logu, potem dane).
- **Księgowanie** (`fs/ext/Journal.*`, JBD2): przy montowaniu sterownik uruchamia `recoverJournal()` —
  odtwarza wszelkie zatwierdzone transakcje pozostawione przez nieczyste zamknięcie i kasuje flagę
  `INCOMPAT_RECOVER`. Każda modyfikująca operacja jest opakowana przez `txFlush()`, który zapisuje
  zmienione metadane do journala, robi checkpoint do ich docelowego miejsca, po czym resetuje log —
  dzięki czemu obraz na dysku pozostaje spójny.
- **Weryfikacja przy starcie:** `extRwSelftest()` w `kernel/Kernel.cpp` zapisuje plik-znacznik do
  `/disks/main/nanos/rwtest`, odczytuje go z powrotem i raportuje, czy znacznik z poprzedniego
  startu przetrwał — przechodząc przez całą ścieżkę VFS → zapis ext + JBD2 → zapis ATA → dysk. Obraz
  jest **e2fsck-clean** po tych zapisach.
