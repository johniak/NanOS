# NanWM — system okienkowy NanOS

NanWM to stos GUI systemu NanOS: userlandowy **kompozytor** (`nwm`), który posiada framebuffer i
urządzenia wejściowe, biblioteka kliencka (`libnw.ndl`), której aplikacje używają do otwierania okien i rysowania, oraz
toolkit widżetów (`libnwui.ndl`) na wierzchu. Wszystko jest zwykłym procesem `.nxe` w ring 3 rozmawiającym przez potoki —
w jądrze nie ma żadnego kodu GUI poza framebufferem `/dev/fb0` i węzłami evdev `/dev/input*`.

Podział odzwierciedla klasyczny OS:

| NanWM | Odpowiednik z Windows | Czym jest |
|---|---|---|
| `nwm` | display server / DWM | posiada ekran + wejście, komponuje okna |
| `libnw.ndl` | `user32` + `gdi32` | connect, utwórz okno, rysuj piksele, pompuj zdarzenia |
| `libnwui.ndl` | `comctl32` / Qt | komponowalne widżety, layout, dispatch zdarzeń |

```
/dev/fb0 (mmap) ─┐                         ┌─ app (e.g. nwnote)  →  libnw  ──┐
/dev/input0 (kbd)┤   nwm  (compositor)     │  app (e.g. nwform)  →  libnwui →┤  libnw
/dev/input1 (mouse)─►  poll → composite ───┤      pipe fds 3 (req) / 4 (evt) │
                       → blit to /dev/fb0   └─ app (rustform)    →  libnwui ──┘ (Rust via C FFI)
```

Format pikseli wszędzie to **32-bpp BGRX** (`0x00RRGGBB` little-endian). Po mechanizm `.ndl`
patrz nxe-ndl.md; po stronę urządzeń `/dev/fb0` + `/dev/input*` patrz
filesystem.md (a sterownik myszy PS/2 to kext, kext.md).

---

## 1. Kompozytor `nwm`

`user/nwm/` — pojedynczy userlandowy `.nxe` (`/nanos/bin/nwm`), uruchamiany przez wpisanie `nwm` w powłoce.
Natychmiast spawnuje demonstracyjny pulpit (Terminal, Settings, Files) i działa do wyjścia ostatniego
klienta GUI, po czym przywraca konsolę tekstową. Kod jest podzielony MI/MD pod testy hostowe:

- **`nwm.c`** — powłoka I/O: jedyna część dotykająca sprzętu (fb + wejście + potoki klientów).
- **`nwm_core.c`** — czysty stan kompozytora: lista okien, z-order, hit-test, dekodowanie wejścia, damage,
  ringi wyjściowe klientów. Bez I/O — testowane na hoście.
- **`nw_compose.c`** — renderowanie sceny + compositing (tapeta, dekoracje, blend blur/glass).

### Posiadanie ekranu i wejścia

Na starcie `nwm` otwiera `/dev/fb0`, odpytuje geometrię przez `ioctl(FBIOGET_VSCREENINFO/FSCREENINFO)` i
robi `mmap` framebuffera `MAP_SHARED`; otwiera `/dev/input0` (pary scancode klawiatury) i
`/dev/input1` (mysz evdev `input_event`). Gdy `nwm` działa, rysuje piksele prosto do
zmapowanego framebuffera, przejmując konsolę tekstową fbcon jądra; przy wyjściu konsola wznawia działanie.

### Pętla poll (`nwm.c`)

Pojedynczy `poll(-1)` blokuje na wszystkich wejściach i potoku request/event każdego klienta, a przy każdym wybudzeniu:

1. **Coalesce** — wpompuj *wszystkie* oczekujące bajty klawiatury, myszy i requestów klientów do stanu
   (`nw_key`, `nw_pointer`, `nw_client_msg` w `nwm_core`) przed jednokrotnym narysowaniem. Zdarzenia REL/BTN myszy
   akumulują się i odpalają na `EV_SYN`.
2. **Reconcile** — zaalokuj bufory content + frame-cache dla nowo utworzonych okien.
3. **Present** — jeśli scena jest dirty: prze-renderuj tylko dirty *ramki okien*, prze-komponuj tylko
   uszkodzony region ekranu, zrób `memcpy` tego prostokąta do framebuffera, potem nałóż kursor.
4. **Flush events** — wypompuj ring wyjściowy każdego klienta do jego potoku zdarzeń (porzuć klienta, którego ring
   się przepełnia).

### Śledzenie damage + frame cache per-okno (rdzeń wydajności)

Kompozytor nigdy nie przemalowuje całego ekranu, jeśli może tego uniknąć:

- **Damage sceny** to pojedynczy bounding box będący unią; `present()` prze-komponuje tylko ten prostokąt i blituje
  tylko ten prostokąt.
- **Frame cache per-okno**: *chrome+content* okna jest renderowane raz do lokalnego dla okna bufora cache;
  compositing czyta z cache. COMMIT, CREATE lub zmiana fokusu ustawia oknu
  `frame_dirty` (titlebar przygasa przy utracie fokusu) — ale **przesunięcie okna (zmiana x/y) NIE**,
  więc drag to tylko *memcpy tapety + composite-from-cache + blit*, z zerowym prze-renderowaniem chrome.
  To (plus szybkie ścieżki rasteryzera z §3) dało ~10× przyspieszenie draga zapisane w
  `../superpowers/plans/2026-06-11-gui-performance.md`.

### Compositing i wygląd glass (`nw_compose.c`)

Warstwa bazowa to cache'owana proceduralna **tapeta** (blobki radial-gradient nad gradientem
diagonalnym). Każde okno komponuje się nad nią z **zaokrąglonymi, antyaliasowanymi rogami** i per-okno
**alpha blendem** (≈234/236) dla półprzezroczystego materiału „glass"; półprzezroczysty **górny pasek menu** i
**dock** oraz obrysowany **kursor** rysowane są na końcu. Blend używa **RB-paired `nw_blend8`** (czerwony
i niebieski mnożone razem, zielony osobno → 2 mnożenia/piksel zamiast 6), z `a`
znormalizowanym do 0..256, tak że w pełni nieprzezroczyste i w pełni przezroczyste są dokładne. Kompozytor rysuje wszystkie
**dekoracje** okna (titlebar z gradientem, boksy close/min) — klienci dostarczają tylko piksele treści.

---

## 2. Protokół klient↔kompozytor (`user/libnw/nwproto.h`)

**Transport.** Strumień bajtów po odziedziczonej **parze potoków**: klient pisze requesty na **fd 3**
i czyta zdarzenia na **fd 4**. `nwm` tworzy dwa potoki, robi `dup2` na 3/4 i `exec`uje
klienta (`nwm.c:132`), więc klient po prostu używa fd 3/4 — żadnego gniazda, żadnego wyszukiwania nazwy.

**Ramkowanie.** Stały **28-bajtowy nagłówek** `nw_msg { type, window, a, b, c, d, length }` (wszystkie 4-bajtowe,
z `static_assert`em na 28) opcjonalnie po którym następuje `length` bajtów payloadu. Ponieważ ring potoku jądra
ma tylko 4096 B, COMMIT pikseli okna przychodzi w wielu fragmentach, więc obie strony dekodują
**strumieniową maszyną stanów** (`nw_decoder`), która składa nagłówek-potem-payload przez wiele odczytów i
nigdy nie blokuje na częściowej wiadomości.

**Współdzielenie bufora to kopia, nie pamięć dzielona.** Klient renderuje do *własnego* bufora pikseli okna
i wysyła uszkodzone prostokąty jako wiadomości COMMIT; kompozytor składa je do bufora
treści okna. Przemalowanie większe niż `NW_COMMIT_MAX_BYTES` (256 KB) jest dzielone na poziome pasma
wierszy, tak by zmieściło się w buforze re-asemblacji per-klient kompozytora.

| Klient → serwer (`NW_REQ_*`) | Argumenty |
|---|---|
| `HELLO` (1) | handshake wersji |
| `CREATE_WINDOW` (2) | `a=w b=h`, payload = tytuł |
| `COMMIT` (3) | `window`, `a=x b=y c=w d=h`, payload = `c*d*4` pikseli BGRX |
| `DESTROY_WINDOW` (4) | `window` |
| `SET_CLIPBOARD` (5) / `GET_CLIPBOARD` (6) | tekst schowka we/wy |
| `SPAWN` (7) | payload = komenda do uruchomienia (ścieżka Run) |
| `SET_MENU` (8) | payload = spec menu aplikacji (menu rozdzielane `0x1e`, pola `0x1f`) |

| Serwer → klient (`NW_EVT_*`) | Argumenty |
|---|---|
| `CONFIGURE` (64) | `a=w b=h` przydzielony rozmiar (wraz z pierwszym mapowaniem) |
| `KEY` (65) | `a=ascii b=down c=scancode d=mods` |
| `POINTER` (66) | `a=x b=y` (względem okna) `c=buttons` |
| `FOCUS` (67) | `a=1/0` |
| `CLOSE` (68) | użytkownik poprosił o zamknięcie |
| `COPY` (69) / `PASTE` (70) | Super+C/X → odpowiedź SET_CLIPBOARD; Super+V → payload = tekst |
| `MENU` (71) | `a=top-menu b=item` wybrane z paska menu |

---

## 3. `libnw.ndl` — biblioteka kliencka (`user/libnw/`)

„user32/gdi32" NanWM (`libnw.h`), importowana po nazwie. Minimalna aplikacja:

```c
nw_display *d   = nw_connect();                          /* fds 3/4 */
nw_win     *win = nw_create_window(d, 400, 300, "Hello");/* blocks for CONFIGURE */
struct nw_surface s; nw_win_surface(win, &s);            /* the window's pixel buffer */
nw_fill_rect(&s, 0, 0, s.w, s.h, 0x202830);
nw_text(&s, 16, 16, "hello from NanWM", 0xE0E0E0);
nw_commit(win, 0, 0, s.w, s.h);                          /* push damage */
struct nw_event ev;
while (nw_next_event(d, &ev, -1) == 1) {                 /* GetMessage/DispatchMessage */
    if (ev.type == NW_EV_CLOSE) break;
    /* NW_EV_KEY / NW_EV_POINTER / NW_EV_FOCUS / NW_EV_PASTE / NW_EV_MENU … */
}
```

Inne punkty wejścia: `nw_set_clipboard`/`nw_get_clipboard`, `nw_spawn` (uruchom program),
`nw_set_menu` (zadeklaruj menu aplikacji) oraz `nw_event_fd` (by klient taki jak terminal mógł `poll`-ować
kompozytor obok własnych fd, np. mastera pty, a potem opróżniać przez `nw_next_event(d, ev, 0)`).

### `nw_gfx` — rasteryzer (`nw_gfx.h/c`)

Czyste, w pełni klipowane rysowanie programowe na `nw_surface { px, w, h, stride, clip_* }` — ten sam
kod, którego kompozytor używa do dekoracji, a klienci do treści. Font to **public-domain
bitmapa 8×16 IBM VGA** (`nx_font8x16`, współdzielona z terminalem `nterm`). Prymitywy: `nw_put_pixel`,
`nw_fill_rect`, `nw_blit`, `nw_draw_char`/`nw_text`/`nw_draw_text` oraz helpery compositingu
`nw_blend_rect` (alpha), `nw_vgrad_rect` (gradient), `nw_fill_round`/`nw_stroke_round` (zaokrąglone prostokąty AA). Opcjonalny scissor per-surface (`nw_surface_clip`) to sposób, w jaki kompozytor prze-komponuje tylko
uszkodzony region. Gorące ścieżki używają blitów `memcpy` per-wiersz i blendu RB-paired.

---

## 4. `libnwui.ndl` — toolkit widżetów (`user/libnwui/`)

„comctl32/Qt" (`nwui.h`): widżety po stronie klienta renderujące do bufora okna libnw aplikacji —
kompozytor pozostaje głupim serwerem pikseli. Model to **komponowalne drzewo węzłów** (kontenery zagnieżdżają
dzieci), silnik **flex-lite layout** (pomiar bottom-up, ułożenie top-down, wagi osi głównej)
oraz **jednokierunkowy przepływ danych** (zdarzenie → callback → zmiana stanu → przemalowanie), z damage per-węzeł
zasilającym ścieżkę damage libnw. Znów podział MI/MD: `nwui_core.c` (drzewo, layout, hit-test, routing
zdarzeń — testowane na hoście), `nwui_paint.c` (renderowanie), `nwui.c` (powłoka I/O).

ABI to celowo **czyste C** (uchwyty opaque, POD, callbacki function-pointer), więc każdy język
z C FFI może budować aplikacje NanWM — `rustform` to to samo demo napisane w Rust.

```c
nwui *u = nwui_open("Form", 360, 200);                 /* does connect + create_window */
nwui_node *root = nwui_column(u,
    nwui_label(u, "Name:"),
    nwui_textfield(u, namebuf, sizeof namebuf, on_change, 0),
    nwui_button(u, "Greet", on_click, u),
    (nwui_node*)0);
nwui_set_root(u, nwui_pad(root, 12));
nwui_run(u);                                            /* event loop until closed */
```

- **Widżety**: `nwui_label`, `nwui_button`, `nwui_textfield` (nad buforem należącym do aplikacji),
  `nwui_list` (+ `nwui_list_set`/`nwui_list_selected`, pojedyncze kliknięcie zaznacza, podwójne kliknięcie/Enter
  aktywuje, scrollbar), `nwui_image`.
- **Kontenery**: wariadyczne `nwui_column`/`nwui_row`/`nwui_box`; przyjazne FFI `nwui_vbox`/`nwui_hbox`
  + `nwui_add`.
- **Właściwości layoutu** (łańcuchowalne): `nwui_pad`, `nwui_gap`, `nwui_flex`, `nwui_size`, `nwui_colors`.
- **Stan**: `nwui_set_text`/`nwui_get_text` oznaczają węzeł jako dirty → przemalowanie.
- **Menu aplikacji**: `nwui_menu` / `nwui_menu_item` / `nwui_menu_separator` wypełniają globalny
  pasek menu w stylu macOS (tytuł pierwszego menu to nazwa aplikacji); wybór odpala callback pozycji.
- **`nwui_spawn`** uruchamia inny program (np. menedżer plików otwierający aplikację).

---

## 5. Routing wejścia

```
/dev/input0 (kbd scancodes) ─┐                      ┌─ hit-test (nw_hit): CONTENT/TITLE/CLOSE/MIN
/dev/input1 (mouse evdev) ───►  nwm: decode + coalesce │  focus (click-to-focus, Super+Tab cycle)
                                (scancode→ASCII US,     │  drag (grab titlebar → move window)
                                 mouse on EV_SYN)       └─ emit to the target client's event ring
                                                            → libnw event queue → app / widget
```

Kompozytor dekoduje scancode klawiatury do ASCII (układ US, modyfikatory) i scala delty/przyciski
myszy, potem robi **hit-test** najwyższego okna pod kursorem i klasyfikuje region
(content, titlebar, boks close/min). Wciśnięcie titlebara zaczyna **drag** okna; kliknięcie **podnosi +
fokusuje** okno (Super+Tab cykluje fokus). Zdarzenia dla fokusowanego/trafionego okna są kolejkowane na ringu
wyjściowym tego klienta i dostarczane jako `NW_EVT_*`.

### 5.1 Skróty klawiszowe są w stylu macOS — **Cmd (⌘)** — wszędzie

NanOS używa **klawisza Cmd (⌘/Super/"GUI", `meta` w QEMU) do WSZYSTKICH skrótów**, jak macOS — nigdy
Ctrl. Dwie warstwy, obie na Cmd:

- **Skróty systemowe (kompozytora)** — obsługiwane przez `nwm`, działają w każdej apce:
  - **Cmd+C / Cmd+X / Cmd+V** — kopiuj / wytnij / wklej (dostarczane do fokusowanego okna jako zdarzenia
    `COPY`/`PASTE`; pole tekstowe kopiuje tekst, siatka plików kopiuje pliki — te same klawisze,
    zależnie od kontekstu).
  - **Cmd+Q** zamknij · **Cmd+Tab** cykluj okna · **Cmd+M** maksymalizuj · **Cmd+R** Uruchom.
- **Skróty aplikacji (per-okno)** — każde *inne* Cmd+&lt;klawisz&gt; jest przekazywane do fokusowanego okna
  z **bitem modyfikatora Cmd** (`NW_EVT_KEY` `mods` bit1); apka dopasowuje przez `nwui_accel(u, cmd=1, …)`.
  Np. Files — **Cmd+N** Nowy folder, **Cmd+L** pasek adresu; Notatnik — **Cmd+S** Zapisz, **Cmd+O** Otwórz,
  **Cmd+F** Znajdź. Klawisze funkcyjne (F2 zmień nazwę, F5 odśwież) są bez modyfikatora.

Zasada przy pisaniu apki: rejestruj skróty przez `nwui_accel(u, /*cmd=*/1, key, …)`; NIE rejestruj
Cmd+C/X/V (należą do kompozytora — obsłuż zdarzenia COPY/PASTE). Kompozytor śledzi Shift (mods bit0)
i Cmd (mods bit1).

---

## 6. Framebuffer (`/dev/fb0`)

`nwm` to zwykły klient fbdev: `FBIOGET_VSCREENINFO`/`FSCREENINFO` po geometrię, `mmap`
liniowego framebuffera, 32-bpp BGRX z pixel stride `line_length/4`. Trzyma rozmiaru ekranu
**`g_scene`** (skomponowaną klatkę), cache'owaną **`g_wall`** (tapetę) oraz bufor roboczy **`g_scratch`**;
tylko prostokąt damage per-klatka jest kopiowany `memcpy` z `g_scene` do zmapowanego framebuffera, więc
mała zmiana kosztuje mały blit. (Strona framebuffer/fbcon jądra to `Fb0Device`/`Framebuffer.*`;
patrz filesystem.md i stos graficzny.)

---

## 7. Aplikacje i build

| App | Biblioteka | Czym jest |
|---|---|---|
| `nwterm` | libnwui | emulator terminala w oknie (poll-uje mastera pty przez `nw_event_fd`) |
| `nwset` | libnwui | ustawienia systemu |
| `nwexp` | libnwui | eksplorator plików (widżet list, nawigacja po katalogach) |
| `nwform` | libnwui | formularz powitalny (textfield + przyciski) — demo toolkitu |
| `nwabout` | libnwui | okno about |
| `nwnote` | **libnw** (bezpośrednio) | notatka tekstowa + demo schowka |
| `rustform` | libnwui przez FFI **Rust** | ten sam formularz w Rust — dowód, że ABI w C jest language-agnostic |

Przy starcie kompozytor spawnuje `nwterm`, `nwset`, `nwexp` (Files jako ostatnie → na wierzchu + fokus).

**Build (Makefile).** `libnw.ndl` i `libnwui.ndl` budowane są jak każda biblioteka dzielona (→
`/nanos/lib`), wspólne rdzenie `nwproto.o`/`nw_gfx.o` linkują się zarówno do `nwm`, jak i do klientów. `nwm` to
`SYS_PROG` (→ `/nanos/bin`); aplikacje demo to `APP_PROGS` (→ bundle `/apps/<name>/`, dostępne przez
link farm `/bin`). `rustform` to `cargo` `no_std` staticlib pod custom cel `i686-nanos.json`
(`-Z build-std=core,alloc`), linkowany przez crt0 + `libnwui.ndl.a` + `libc.ndl.a`.

**Testy.** Czyste rdzenie są w bramce pokrycia (`TEST_MODULES` / `COV_PATTERNS`):
`nwm_core`, `nw_compose`, `nwproto`, `nw_gfx`, `nwui_core` — wszystkie działają po stronie hosta pod doctest —
dekoder protokołu, layout, hit-test, unia damage i matematyka compositingu są testowane bez
framebuffera.

---

## 8. Zobacz też

- `../superpowers/plans/2026-06-11-gui-performance.md` — diagnoza wydajności draga oraz
  optymalizacje raster/blend/frame-cache (≈10× zysk czasu klatki).
- `.claude/nanoos-ui/` — statyczny mockup HTML koncepcji pulpitu (dock, pasek menu, okna
  glass).
- nxe-ndl.md (biblioteki `.ndl`), kext.md (kext myszy PS/2 zasilający
  `/dev/input1`), filesystem.md (`/dev/fb0`, `/dev/input*`).
