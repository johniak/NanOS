# Plan: wydajność trybu graficznego (NanWM) — szybkie przeciąganie okien bez utraty wyglądu

Plan dla agenta-wykonawcy. Diagnoza zrobiona na kodzie z gałęzi `dockerized-build`
(2026-06-11). Cel: przeciąganie okna i ogólna responsywność desktopu mają być płynne
(≥30–60 fps w QEMU), **bez żadnej zmiany wyglądu** (szkło/translucencja, zaokrąglone
rogi z AA, gradienty, dock, pasek menu — piksel w piksel to samo, poza szybkością).
Commity bez wzmianki o AI. Praca fazami: każda faza = pomiar → zmiana → pomiar →
host-testy → weryfikacja QEMU → commit.

---

## Diagnoza: gdzie ucieka czas przy przeciąganiu

Architektura jest ZDROWA: pętla `nwm.c` koalescuje wejście przed rysowaniem, damage-rect
działa (przy drag unia starej i nowej ramki — ciasna, `nwm_core.c:437-442`), kursor to
tani overlay (`present()`, `nwm.c:277-306`), blur nie jest nigdzie wołany (martwy kod).
Problem to **koszt jednostkowy piksela** pomnożony przez kilka warstw:

1. **CAŁY system kompiluje się bez optymalizacji.** `USER_CFLAGS` (Makefile:218),
   kernelowe `CXXFLAGS` (Makefile:106), `KEXT_CFLAGS` (Makefile:486) i `DOOM_CFLAGS`
   (Makefile:532) **nie mają żadnej flagi `-O`** → wszystko leci na domyślnym -O0.
   Jedyne -O2 w repo ma… hostowy `tools/mknx.c` (Makefile:310). Przy -O0 żadna funkcja
   rastra nie jest inline'owana, a pętle pikselowe mają ~10× narzut. To jest
   **dominujący czynnik** i zarazem najtańsza naprawa.
2. **Przy drag każda klatka renderuje okno OD ZERA.** `nw_compose_scene`
   (`nw_compose.c:264-302`) dla każdej klatki woła `draw_window_to` → pełny fill
   materiału + gradient paska + blit zawartości + tekst tytułu do screen-sized
   `scratch`, a dopiero potem komponuje. Pozycja się zmieniła, treść NIE — a płacimy
   pełny re-render.
3. **Szkło nigdy nie bierze szybkiej ścieżki.** `WIN_ALPHA=234`/`DARK_ALPHA=236` < 255,
   więc „proste środkowe wiersze" w `composite_round` (`nw_compose.c:149-154`) zawsze
   idą przez per-pikselowy `cmix` (6 mnożeń + składanie kanałów na piksel), nigdy przez
   `memcpy`. To koszt nieusuwalny bez utraty wyglądu, ale da się go ~2-3× obniżyć
   (RB-paired blend) i ograniczyć liczbę warstw nad nim.
4. **Rasteryzator ma per-pikselowe wywołania i klipowanie.** `nw_blit`
   (`nw_gfx.c:100-123`) kopiuje pętlą piksel-po-pikselu zamiast `memcpy` wierszami
   (to m.in. tapeta przywracana pod oknem w każdej klatce). `nw_text`/`nw_draw_char`
   (`nw_gfx.c:63-98`) wołają `nw_put_pixel` na KAŻDY piksel glifu, a `nw_put_pixel`
   na każdy piksel od nowa liczy `nw_bounds` (przy -O0: wywołanie + 8 porównań na piksel).
5. **Środowisko mnoży koszty:** QEMU TCG na Apple Silicon emuluje i686 programowo, a
   scheduler (osobny audyt: `2026-06-11-scheduler-audit-fixes.md`) preemptuje co 1 ms
   z bezwarunkowym reloadem CR3 = pełny flush TLB w środku pętli pikselowych po dużych
   buforach.

Rachunek dla okna ~600×400 na ekranie 1024×768: damage ≈ 300 K px, na to tapeta
(per-px pętla), scratch-render ~250 K px (kilka przebiegów), composite ~250 K px × cmix,
blit do fb ~300 K px — łącznie >1 M operacji pikselowych na każdy ruch myszy, wszystko
przy -O0 pod TCG. Stąd „mulenie".

**Czego NIE robić:** nie wyłączać przezroczystości/AA na czas przeciągania (utrata
wyglądu — odrzucone), nie dotykać protokołu COMMIT (drag nie angażuje klientów), nie
przepisywać kompozytora na inną architekturę — obecna (scena + damage + overlay kursora)
jest właściwa.

---

## Faza 0 — pomiar (najpierw liczby, potem zmiany)

1. W `nwm.c` dodać pomiar czasu klatki: `clock_gettime(CLOCK_MONOTONIC)` wokół
   `present()`; zliczać min/avg/max czasu klatki i fps w oknie ~2 s. Wynik wypisywany
   np. co 2 s na stderr (konsola tekstowa pod spodem) albo trzymany w liczniku
   zrzucanym na żądanie (Super+F?) — decyzja wykonawcy, byle dało się odczytać w QEMU.
2. Skrypt testowy drag: `scripts/qemu-drive.py` (jeśli jeszcze nie ma w repo — skopiować
   z `/tmp/bashdrive.py` i scommitować, jak w planie NanWM) z sekwencją: boot → `nwm` →
   `mbtn:` wciśnij na pasku tytułu → seria `mmove:` (np. 30 × delta 10 px) → `mbtn:`
   puść → screendump.
3. Zanotować bazę: czas klatki podczas drag PRZED zmianami. Każda kolejna faza
   raportuje te same liczby. (Pomiar zostaje w kodzie za stałą/`#ifdef`, wyłączony
   domyślnie po zakończeniu prac.)

## Faza 1 — optymalizacja kompilacji (największy pojedynczy zysk)

1. Dodać `-O2` do: `USER_CFLAGS` (Makefile:218), kernelowych `CXXFLAGS` (Makefile:106),
   `KEXT_CFLAGS` (Makefile:486), `DOOM_CFLAGS` (Makefile:532). Do rozważenia dodatkowo
   `-fomit-frame-pointer` (userland) — sprawdzić, czy nic nie chodzi po ramkach stosu.
2. **Bezpieczniki:** dodać `-fno-strict-aliasing` (wzór: Linux) — kod jądra i userlandu
   rzutuje wskaźniki swobodnie (`Registers*`, bufory pikseli, nagłówki NXE) i -O2 z
   strict aliasing może go cicho połamać. Rozważyć też `-fno-delete-null-pointer-checks`.
3. Ryzyka do sprawdzenia po kolei (build + boot QEMU po każdej grupie):
   - inline asm bez pełnych clobberów (grep `__asm__` po jądrze/userlandzie — zweryfikować
     constrainty; szczególnie `int $0x80` wrappery i port I/O),
   - kod zależny od układu ramki stosu / `volatile` brakujące przy MMIO (`0xB8000`,
     LFB — `Framebuffer.cpp`, `console_x86`),
   - `.bss`/sekcje i rozmiary NXE (mknx czyta ELF — upewnić się, że -O2 nie zmienia
     założeń loadera; nagłówek `.nxeheader` ma sekcję jawną, powinno być OK),
   - timing-sensitive pętle czekania w sterownikach (`waitWrite` w kbd/mouse kext —
     pętle zliczane iteracjami zrobią się krótsze CZASOWO; jeśli coś przestanie
     działać, zamienić na pętle po statusie, nie „dokręcać" liczników).
4. Pełny regres: `make test` (host-testy są budowane osobnymi flagami — bez zmian),
   boot QEMU: shell, bash, potoki, sygnały, PTY, desktop nwm, Doom jeśli jest w obrazie.
5. Pomiar fazy 0 ponownie — oczekiwany zysk rzędu kilku ×.

> Uwaga: trzymać -O2, nie -O3 (rozmiar NXE/kernela i przewidywalność; i686-elf gcc
> bez SSE i tak nie zwektoryzuje wiele). ŻADNYCH zmian semantycznych w tej fazie —
> wyłącznie flagi.

## Faza 2 — rasteryzator `nw_gfx.c` (tanie, mierzalne, host-testowalne)

1. **`nw_blit`:** wiersz = `memcpy(drow, srow, w*4)` zamiast pętli po pikselach
   (`nw_gfx.c:117-122`). To przyspiesza tapetę pod oknem i blit zawartości okna.
2. **`nw_text`/`nw_draw_char`:** rozwiązać `nw_bounds` RAZ na wywołanie (nie per
   piksel), pisać bezpośrednio do wiersza; szybka ścieżka „glif w całości w bounds"
   bez żadnych porównań per piksel, wolna ścieżka brzegowa jak dziś.
3. **Blend dwukanałowy (RB-paired):** wspólny helper dla `cmix` (`nw_compose.c:126-133`)
   i `nw_mix` (`nw_gfx.c:127-136`):
   `rb = ((s&0xFF00FF)*a + (d&0xFF00FF)*ia) >> 8 & 0xFF00FF; g analogicznie` —
   2 mnożenia zamiast 6 na piksel. UWAGA na równoważność: `nw_mix` dzieli przez 255,
   `cmix` przez >>8 — wyniki różnią się o ±1 LSB. Ujednolicić na >>8 z korektą
   (`a += a>>7`), żeby 255 dawało dokładnie src. **Host-test równoważności**: dla
   pełnej siatki (s,d,a) ∈ próbkowane wartości, |nowy − stary| ≤ 1 na kanał, oraz
   a=0 → d i a=255 → s bitowo.
4. `nw_fill_rect`/`nw_blend_rect`/`nw_vgrad_rect`: zostawić (proste pętle po -O2 są OK);
   `nw_blur_rect` — martwy kod: oznaczyć komentarzem „unused" albo usunąć (decyzja
   wykonawcy; jeśli redesign UI ma go użyć — zostawić).
5. Host-testy pikselowe już istnieją (`tests/test_nw_gfx.cpp`) — dopisać przypadki na
   nowe ścieżki (blit z klipem po memcpy, tekst na granicy klipu, blend ±1 LSB).

## Faza 3 — cache wyrenderowanych okien (likwiduje re-render przy drag)

Teraz `draw_window_to` renderuje chrome+content do screen-sized `scratch` przy każdej
klatce (`nw_compose.c:80-123,276`). Zamiast tego:

1. Per okno bufor `rendered` (fw×fh, window-local; fw/fh = `frame_w/h`), alokowany
   przy tworzeniu/zmianie rozmiaru okna obok `g_winbuf` w `nwm.c` (`reconcile_buffers`).
   Pamięć: ~1 MB na okno 600×400 × `NW_MAX_WINDOWS` — policzyć i upewnić się, że heap
   userlandu to mieści; jeśli ciasno, alokować leniwie przy pierwszym renderze.
2. Flaga `frame_dirty` w `struct nw_window`, ustawiana przy: COMMIT (zmiana
   zawartości), zmianie fokusu (przyciemnienie paska — `nwm_core.c:111-115`), zmianie
   tytułu/menu, resize. **Zmiana x/y NIE brudzi** — to tylko inna pozycja kompozycji.
3. `nw_compose_scene`: gdy `frame_dirty`, wyrenderuj okno do `rendered`
   (window-local `nw_surface`, bez klipu sceny) i zgaś flagę; następnie
   `composite_round` czyta z `rendered` z indeksowaniem `srow = rendered + yy*stride`
   (dziś czyta screen-space scratch — `nw_compose.c:148`; refaktor indeksów x/y na
   window-local). Screen-sized `g_scratch` znika (oszczędność pamięci ≈ rozmiar
   ekranu) — ścieżka hostowa `scratch==0` w `nw_compose_scene` zostaje jak jest.
4. Drag po tej fazie = na klatkę: tapeta (memcpy z fazy 2) + composite_round z cache
   (RB-paired cmix) + blit do fb. Zero fill/gradient/tekst/glifów.
5. Host-testy `test_nwm_core`/nowy `test_nw_compose`: scena z cache bitowo równa
   scenie z pełnego renderu (zlecić porównanie compose-z-dirty vs compose-świeże dla
   tych samych stanów, w tym po zmianie fokusu i COMMIT).

## Faza 4 — pacing klatek przy przeciąganiu (drobna, po pomiarach)

Koalescencja już ogranicza liczbę klatek do tempa pętli, ale gdy compose stanieje,
pętla może rysować setki klatek/s na szybkie delty myszy — niepotrzebnie:

1. W `nwm.c` przed `present()`: jeśli scena dirty wyłącznie z powodu ruchu
   przeciągania i od ostatniego presentu minęło < ~12-16 ms (`clock_gettime`),
   pominąć present w tej iteracji (poll i tak zaraz wróci z kolejnym wejściem;
   damage się unifikuje). Kursor sam w sobie jest tani — limitować tylko recompose.
2. Nie wprowadzać żadnego „trybu przeciągania" zmieniającego rendering — wygląd
   identyczny, zmienia się tylko maksymalna częstotliwość rekompozycji.

## Faza 5 — synergia systemowa (odnośniki, nie duplikować pracy)

Z planu `2026-06-11-scheduler-audit-fixes.md` na płynność GUI bezpośrednio działają:
warunkowy reload CR3 (P3 — mniej flushy TLB w środku pętli pikselowych), kwant 10 ms
(P5 — rzadsze przełączenia), wait-queues (P1 — nwm budzi się od zdarzeń, nie co tyk).
Jeśli tamten plan nie wszedł przed tym — wykonać najpierw jego fazę 4 (P2-P4), bo
poprawia pomiary tutaj. Dla jakości odświeżania OKIEN klientów (nie draga) rozważyć
na końcu podbicie `Pipe::CAP` (4096 → 64 K, `kernel/Pipe.h:55`) — COMMIT-y przestaną
mielić dziesiątki cykli pipe-refill na klatkę zawartości.

---

## Kryteria akceptacji

1. **Pomiar:** czas klatki podczas syntetycznego drag-testu (faza 0) spada co najmniej
   ~10× względem bazy; cel bezwzględny ≥ 30 fps w QEMU TCG na maszynie deweloperskiej
   (idealnie ~60).
2. **Wygląd bez zmian:** screendumpy desktopu (statyczna scena: dwa okna, dock, pasek,
   menu otwarte) przed i po — różnice pikselowe ≤ 1 LSB na kanał wyłącznie w obszarach
   blendowanych (skutek RB-paired), zero różnic strukturalnych (rogi, AA, cienie,
   translucencja obecne).
3. `make test` zielone, pokrycie gated modules ≥ 90% (nowe testy rasteryzatora i
   równoważności cache wliczone do `COV_PATTERNS`).
4. Pełny regres QEMU: drag, fokus, Super+Tab, menu, Run, terminal okienkowy (pisanie,
   kolory), schowek, zamykanie okien, wyjście z nwm do konsoli, brak `v=08/0d/0e`.
5. Brak regresji poza GUI po -O2: bash, potoki, sygnały, job control, Doom (jeśli
   obecny), boot bez zmian.

## Kolejność i szacunek ryzyka

| Faza | Zysk | Ryzyko | Uwagi |
|------|------|--------|-------|
| 0 pomiar | — | zerowe | warunek sensownej reszty |
| 1 -O2 | największy (kilka ×) | średnie (UB/asm) | bezpieczniki: -fno-strict-aliasing; regres całości |
| 2 raster | duży | małe | czysto lokalne, host-testy |
| 3 cache okien | duży przy drag | średnie (refaktor compose) | testy równoważności scen |
| 4 pacing | mały | małe | dopiero po pomiarach |
| 5 scheduler/pipe | średni | wg tamtego planu | nie duplikować |

Fazy 1 i 2 są niezależne od 3; robić w tej kolejności (po każdej liczby z fazy 0
mówią, czy schodzić niżej). Jeśli po fazach 1-3 drag osiąga cel, fazę 4 można pominąć.

---

## Status wykonania (2026-06-12)

- **Faza 0 — ZROBIONA** (commit 80d9f9e). Pomiar czasu klatki w `nwm.c` za stałą `NWM_PROFILE`
  (domyślnie 0), mikrosekundy całkowite (bez float printf), na stderr co ~2 s.
- **Faza 1 — ZROBIONA dla USERLANDU** (commit 9118316). `UOPTFLAGS=-O2 -fno-strict-aliasing
  -fno-delete-null-pointer-checks` na `USER_CFLAGS` + `DOOM_CFLAGS`. Cały gorący raster
  (nw_gfx/nw_compose) jest w userlandzie piszącym mmapowany framebuffer, więc to łapie zysk z draga.
  **JĄDRO zostaje na -O0** (`KOPTFLAGS` puste): przy -O2 ujawnia się utajony UB w skanie ścieżki
  w sterowniku ext (pętla basename/dirname czyta poza nie-NUL-zakończonym buforem `String`) →
  triple fault przy montowaniu roota. To osobne zadanie (naprawa UB w FS); kompozycja nie używa
  gorącego kodu jądra, więc nic nie tracimy na wydajności GUI. **Follow-up: naprawić ten UB, potem
  włączyć -O2 dla jądra/kext.**
- **Faza 2 — ZROBIONA** (commit a99f581). `nw_blit` = memcpy wierszami; `nw_draw_char`/`nw_text`
  rozwiązują klip raz + szybka ścieżka „glif w bounds"; wspólny `nw_blend8` (RB-paired, >>8 z
  korektą `a+=a>>7`) dla `nw_mix` i `cmix`. Host-testy równoważności ±1 LSB. Wizualnie tylko piksele
  blendowane drgają ≤1 LSB; piksele nieprzezroczyste bit-identyczne.
- **Faza 3 — ZROBIONA** (commit b61fa9e). Per-okno cache `frame` (window-local) + `frame_dirty`;
  brudzony przy COMMIT/zmianie fokusu/utworzeniu, **nie** przy ruchu (x/y). `nw_render_dirty_frames`
  odświeża brudne ramki przed kompozycją; `composite_round` czyta ramkę window-local. Drag =
  tapeta + glass-composite z cache + blit, zero re-renderu chrome/treści. Host-test: kompozycja z
  cache **bit-identyczna** z kompozycją na żywo; ruch nie brudzi ramki. QEMU: pulpit piksel-w-piksel
  jak przed (diff 0).
- **Faza 4 — POMINIĘTA** świadomie (zgodnie z planem: opcjonalna, „mały zysk", do pominięcia jeśli
  1-3 wystarczą). Bezpieczne pacing wymaga przerobienia `poll(-1)` na sterowane timeoutem odraczanie
  (inaczej ostatnia klatka draga zalegałaby do następnego wejścia) — realne ryzyko regresji
  (judder/zaleganie) dla marginalnego zysku przy modelu poll-blocking, gdzie tempo i tak ogranicza
  wejście. Do rozważenia tylko jeśli pomiar fazy 0 pokaże, że recompose zalewa wyświetlanie.
- **Faza 5 — odnośnik**, nie duplikowana (osobny plan schedulera; podbicie `Pipe::CAP` opcjonalne).

Bramki: `make test` zielone (91.5%, dodane testy raster + równoważności cache), `make check-arch`
czysty, QEMU bez `v=08/0d/0e`, pulpit bit-identyczny (tylko ≤1 LSB w obszarach blendowanych z Fazy 2).
