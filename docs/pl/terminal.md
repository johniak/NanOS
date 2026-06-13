# Konsola, TTY i terminal w NanOS

Ścieżka tekstowa od dziennika rozruchu jądra do interaktywnej powłoki: niezależna od maszyny
**konsola** nad wymienialnym ujściem (tekst VGA → glify framebuffera), **klawiatura evdev**
(`/dev/input0`), **PTY** (`/dev/ptmx` + `/dev/pts0` + `/dev/tty`) z line discipline + sygnałami
kontroli zadań oraz współdzielony **silnik VT/ANSI** używany zarówno przez framebufferową konsolę
jądra, jak i terminale w przestrzeni użytkownika (`nterm`, `nwterm`). Kompozytor GUI opisuje
windowing.md; `/dev/fb0` + `/dev/input*` w przestrzeni nazw opisuje filesystem.md.

---

## 1. Konsola jądra (`drivers/Console.*` + `<arch/console.h>`)

`Console` to warstwa formatowania MI (`write`/`writeLine`/`writeHex`, dziesiętne/szesnastkowe itoa,
kursor, przewijanie), która napędza **ujście** komórek znakowych przez kontrakt arch:
`consolePutChar`, `consoleClear`, `consoleSetCursor`, `consoleInit`, `consoleActivateFramebuffer`,
`consoleSize` (ostatnie raportuje siatkę dla `TIOCGWINSZ`). Całe wyjście rozruchowe jądra przechodzi
tędy.

---

## 2. Przełącznik ujścia: tekst VGA → konsola framebuffera

Ujście x86 (`arch/x86/drivers/console_x86.cpp`) startuje w **trybie tekstowym VGA**: siatka komórek
80×25 pod `0xB8000` (znak + bajt atrybutu), sprzętowy kursor przez porty `0x3D4/0x3D5`, przewijanie
przez `memcpy` 24 wierszy w górę. `consolePutChar` interpretuje `\b \t \r \n`.

Gdy framebuffer rozruchowy jest zmapowany (boot.md §4), `consoleActivateFramebuffer()` przełącza
`g_useFb`, tak że każde wywołanie ujścia kieruje się zamiast tego do **konsoli framebuffera**
(`drivers/FbConsole.*`) — splash rozruchowy `[ OK ]` od tego momentu renderuje się jako glify
pikselowe. `FbConsole` jest zbudowany na współdzielonym **silniku VT** (§6) jako jego siatka + parser
sekwencji escape, trzyma siatkę-cień, aby przemalowywać tylko zmienione komórki, i rysuje glify 8×16
z podkreślającym kursorem. `consoleSize` zwraca wtedy wymiary siatki fbcon zamiast 80×25.

---

## 3. Framebuffer (`drivers/Framebuffer.*`, `Fbdev.*`, `Fb0Device.*`)

`FbSurface { base, pitch, width, height, bpp }` + czyste rasteryzery (`fbPutPixel`, `fbFillRect`,
`fbBlitGlyph`, `fbScrollUp`) — testowalne na hoście renderowanie programowe, BGRX (32bpp) lub BGR
(24bpp). Dwóch konsumentów:

- **fbcon jądra** (`FbConsole`) rysuje glify wprost do zmapowanego framebuffera na potrzeby dziennika
  rozruchu;
- **`/dev/fb0`** (`Fb0Device` nad `Fbdev`) to linuksowe urządzenie znakowe fbdev: ioctle
  `FBIOGET_VSCREENINFO`/`FSCREENINFO`, `read`/`write` oraz `mmap` liniowego framebuffera. `nwm` i
  `nterm` otwierają go, `mmap`ują i przejmują ekran (windowing.md §6).

Framebuffer rozruchowy (addr/pitch/w/h/bpp) jest odkrywany z informacji Multiboot
(`bootFramebuffer`), żądany przez flagę VIDEO Multiboot (boot.md §1). Bez niego konsola pozostaje w
trybie tekstowym VGA.

---

## 4. Klawiatura / evdev (`/dev/input0`)

PS/2 IRQ1 należy do **kext kbd** (kext.md), który podaje scancode do jądra przez
`arch::inputFeedScancode` → idzie to **dwiema drogami naraz**:

1. **`/dev/input0`** (`drivers/KeyboardDevice.*`): evdev w stylu Linuksa — pierścień 2-bajtowych
   zdarzeń `[code][down]` (bit 7 = rozszerzony/`0xE0`), czytanych w całości, nieblokujących
   (`-EAGAIN`, gdy pusto). To czytają klienci GUI i `nterm`.
2. **konsola cooked** (`KeyDecoder` → line discipline): scancode→ASCII (zestaw-1, układ US,
   Ctrl/Shift), strzałki → sekwencje ANSI, a klawisze sterujące podnoszą sygnały — `Ctrl+C`→SIGINT,
   `Ctrl+\`→SIGQUIT, `Ctrl+Z`→SIGTSTP przez `consoleSignal`. `termmode`(501) przełącza to między
   cooked (canonical + echo + sygnały) a raw (bajty na wprost). Zablokowani czytelnicy budzą się
   przez kolejkę oczekiwania (scheduler.md §4).

---

## 5. PTY (`drivers/Pty.*`) — `/dev/ptmx`, `/dev/pts0`, `/dev/tty`

Para pseudoterminala: **master** (`/dev/ptmx`, trzymany przez emulator terminala) i **slave**
(`/dev/pts0`, sterujące tty powłoki; aliasowane też jako `/dev/tty`). Dwa pierścienie bajtów —
`m2s` (wejście) i `s2m` (wyjście) — plus **line discipline** po stronie wejścia:

- domyślne termios: canonical (`ICANON`), echo, sygnały (`ISIG`), `ICRNL` na wejściu / `ONLCR` na
  wyjściu.
- znaki sterujące: `VINTR`=Ctrl+C, `VQUIT`=Ctrl+\, `VSUSP`=Ctrl+Z, `VERASE`=DEL, `VEOF`=Ctrl+D.
- zapis do mastera stosuje line discipline bajt po bajcie: `VINTR`/itp. woła funkcję sygnału
  (`ptySignal` → `consoleSignalGroup`), by dostarczyć ją do **pierwszoplanowej grupy procesów**;
  tryb canonical buforuje wiersz (obsługując erase/EOF), echuje go i opróżnia do slave'a przy nowej
  linii; wyjście slave'a dostaje obróbkę `ONLCR`.
- ioctle: `TCGETS/TCSETS{,W,F}` (termios), `TIOCGWINSZ/TIOCSWINSZ` (rozmiar), `TIOCGPGRP/TIOCSPGRP`
  (pierwszoplanowa pgrp — kontrola zadań), `TIOCPKT` (tryb pakietowy, dla telnetd). Master odmawia
  `TIOC*PGRP`, więc emulator sam nie jest terminalem sterującym.

**Kontrola zadań:** powłoka robi `tcsetpgrp` na pierwszoplanowej grupie; `Ctrl+C` na masterze kieruje
wtedy SIGINT dokładnie do tej grupy (scheduler.md §5). Stroną użytkownika są `termios.c` (tcgetattr/
tcsetattr/cfmakeraw/tcsetpgrp) i `ptyutil.c` (`openpty`/`forkpty`/`login_tty` dla telnetd).

---

## 6. Silnik VT/ANSI (`user/term/vt.*`)

**Czysta** maszyna stanów VT100/xterm — bez I/O, bez globalnych zmiennych, testowana na hoście —
**współdzielona** przez fbcon jądra (`FbConsole`) i terminale przestrzeni użytkownika (`nterm`,
`nwterm`). Trzyma siatkę komórek (`ch, fg, bg`), kursor, atrybuty SGR (16 bazowych + 256 kolorów +
paleta truecolor), region przewijania, zapisany kursor, **ekran alternatywny** (DECSET 47/1047/1049)
oraz bitmapę **dirty** per wiersz, aby renderer przemalowywał tylko zmienione wiersze.
`vt_feed(bytes)` uruchamia parser (NORMAL/ESC/CSI/OSC): drukowalne → glif; finały CSI → ruchy
kursora, kasowanie (`J`/`K`), `SGR` (`m`), region przewijania (`r`), save/restore, zamiana ekranu
alternatywnego; OSC pochłaniane do BEL/ST. Czcionka to `vtfont.c` (8×16, współdzielona z `nw_gfx`
biblioteki libnw).

---

## 7. Terminale: `nterm` vs `nwterm`

- **`nterm`** (`user/term/nterm.c`) — terminal **pełnoekranowy**: `mmap`uje `/dev/fb0` bezpośrednio,
  wymiaruje siatkę `vt` do framebuffera, otwiera `/dev/ptmx`, `fork`uje powłokę na `/dev/pts0`
  (`TERM=xterm-256color`), a jego pętla `poll` pompuje master→`vt_feed`→render-dirty oraz
  klawiaturę(`/dev/input0`)→master. To terminal, gdy nie ma kompozytora.
- **`nwterm`** (`user/nwterm/`) — terminal **w oknie**: ten sam silnik `vt` + pty + powłoka, ale
  klient NanWM rysujący do okna przez `libnw` i odpytujący event fd kompozytora obok mastera pty
  (windowing.md §7).

Oba używają dokładnie tego samego silnika VT i czcionki; różnią się jedynie miejscem docelowym
pikseli i źródłem wejścia.

---

## 8. Połączenia rozruchowe

`Kernel::start` rejestruje węzły urządzeń (boot.md §4): `/dev/fb0` (`Fb0Device`, jeśli jest
framebuffer), `/dev/input0` (`KeyboardDevice` + `kbdRegister`, aby ścieżka IRQ go zasilała),
`/dev/ptmx` + `/dev/pts0` + `/dev/tty` (jeden `Pty`, z `ptySignal` podpiętym dla Ctrl+C → pgrp).
Konsola jądra (VGA→fbcon) niesie dziennik rozruchu; gdy `init` uruchomi powłokę, interakcja idzie
przez PTY (oraz `nterm`/`nwterm` dla pełnego terminala).

**Kluczowe pliki:** `drivers/{Console,FbConsole,Framebuffer,Fbdev,Fb0Device,KeyboardDevice,Pty}.*`,
`arch/x86/drivers/{console_x86,input_x86}.cpp`, `kernel/KeyDecoder.*`,
`user/term/{vt.*, vtfont.c, nterm.c}`, `user/libc-glue/{termios.c, ptyutil.c}`.
