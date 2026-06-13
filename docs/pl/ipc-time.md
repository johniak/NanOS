# Potoki i czas w NanOS

Dwa małe, ale nośne elementy jądra: **potok** (byte FIFO stojące za `pipe(2)` oraz model ringu, z
którego korzysta PTY) i **zegar ścienny** (epoka bootu z RTC + monotoniczny tick schedulera). Oba
są machine-independent i host-tested; blokowanie i odczyt RTC żyją poza nimi.

---

## 1. Potoki (`kernel/Pipe.h`)

`Pipe` to **byte FIFO** na ringu o stałym rozmiarze, z osobnymi refcountami otwartych końców —
obiekt stojący za parą `pipe(2)` i model, z którego korzysta PTY (@docs/terminal.md §5). To **czysta
struktura danych**: bez zależności od schedulera/arch, więc jest host-tested (`tests/test_pipe.cpp`);
blokowanie to zadanie dispatchu, nie potoku.

- **Ring** — `write(src, n)` / `read(dst, n)` przenoszą do `n` bajtów i zwracają liczbę przeniesioną
  (`0` = pełny / pusty). `CAP = 64 KiB` — celowo *nie* 4 KiB: framebuffer COMMIT aplikacji okienkowej
  (pełny redraw to ~770 KiB pikseli przez pipe żądań, @docs/windowing.md §2) odbijałby się ~190 razy
  od ringu 4 KiB, a każda blokada kosztuje rundę schedulera (zabójcze pod QEMU TCG); ring 64 KiB tnie
  liczbę rund ~16×. Potoki są alokowane na stercie i nieliczne, więc te bajty są tanie.
- **EOF przez refcounty** — `addReader/addWriter/dropReader/dropWriter` śledzą otwarte końce
  (`pipe()` otwiera po jednym z każdego, `dup()` zwiększa, `close()` zmniejsza). `atEof()` = opróżniony
  **i** brak pozostałego pisarza, więc odczyt z pustego potoku zwraca `0` (EOF) dopiero gdy wszystkie
  końce zapisu są zamknięte — w przeciwnym razie wołający się blokuje.
- **Blokowanie** — `waitQueue()` udostępnia `WaitQueue`, gdzie parkują czytelnicy (pusty) / pisarze
  (pełny); dispatch budzi ich po każdym read/write/close zmieniającym gotowość, zamiast odpytywać co
  tick (@docs/scheduler.md §4).

Deskryptor potoku to jeden z jednolitych backingów fd w rdzeniu `Syscalls` (plik / potok / gniazdo /
pty), więc `read`/`write`/`close`/`dup`/`poll` działają na nim jak na każdym innym fd
(@docs/syscalls.md §2).

---

## 2. Czas i zegar ścienny (`kernel/Clock.h`)

NanOS ma dwa pojęcia czasu:

- **Tick monotoniczny** — `Scheduler::ticks()`, licznik timera 1000 Hz od bootu (@docs/scheduler.md
  §3). Napędza `nanosleep`, timeouty poll/select, timery TCP oraz `/proc/uptime`.
- **Zegar ścienny** — zasiewany raz przy starcie: `setBootEpoch(arch::rtcEpoch())` próbkuje RTC
  platformy (x86: CMOS RTC, `arch/x86/cpu/cpu_x86.cpp`, za `<arch/cpu.h>`), a `wallClockSeconds()`
  zwraca `bootEpoch + ticks/1000`. Żyje w warstwie MI, żeby kod filesystemu mógł znakować inody
  czasem (`mtime`/`ctime` przy zapisie, `utimes` „teraz") bez sięgania do arch (@docs/filesystem.md
  §5). Zwraca 0, dopóki epoka nie jest ustawiona (np. testy hosta bez timera) — nieszkodliwe dla
  timestampów na dysku.

Powierzchnie:

- **`clock_gettime`(265)** zwraca czas do userlandu (@docs/syscalls.md); zegar userlandu jest
  monotoniczny i zasila picolibc / portowane aplikacje (fix monotonicznego `clock_gettime` był
  potrzebny dla portów sieciowych).
- **`/proc/uptime`** = `ticks/1000`; **`/proc/stat` `btime`** = teraz (RTC) − uptime
  (`fs/SynthFs.cpp`).
- **`getrandom`(355)** i CSPRNG również mieszają RTC/tick przy zasiewaniu (`kernel/Csprng.cpp`).

**Kluczowe pliki:** `kernel/Pipe.h`, `kernel/Clock.h`, `arch/x86/cpu/cpu_x86.cpp` (`rtcEpoch`),
`fs/SynthFs.cpp` (uptime/stat), `tests/test_pipe.cpp`.
