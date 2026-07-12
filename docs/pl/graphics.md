# Grafika NanOS i sterownik virtio-gpu na LinuxKPI

Ten dokument opisuje, **skąd bierze się obraz**: jak jest podłączony `/dev/fb0` oraz warstwę
**LinuxKPI**, która uruchamia **niezmodyfikowany sterownik GPU z Linuksa**, żeby go napędzić.
Kompozytor i toolkit, które *rysują* do `/dev/fb0`, opisuje [windowing.md](windowing.md); mechanizm
modułów ładowalnych — [kext.md](kext.md); multipleksowanie konsoli na VT — [x86_64.md](x86_64.md) §5.

---

## 1. Dwa źródła `/dev/fb0`

NanOS rysuje wszystko (konsole tekstowe fbcon, a potem pulpit `nwm`) do jednego liniowego
framebuffera wystawionego jako `/dev/fb0`. Może być on podłączony na dwa sposoby:

| Źródło | Kiedy | Jak |
|---|---|---|
| **Framebuffer firmware** | domyślnie — realny sprzęt (UEFI GOP) **oraz** domyślne VGA/VBE w QEMU | bootloader (Limine / GRUB multiboot) przekazuje jądru liniowy framebuffer; `Kernel::start` buduje na nim stos VT + `/dev/fb0` |
| **virtio_gpu na LinuxKPI** | QEMU `-device virtio-gpu-pci` (zwł. z `-vga none`) | `virtio_gpu.nkext` uruchamia niezmodyfikowany sterownik DRM, tworzy scanout i podpina jego bufor pod `/dev/fb0` |

Te ścieżki nigdy się nie kolidują: kext virtio_gpu przejmuje `/dev/fb0` **tylko** wtedy, gdy
faktycznie znajdzie urządzenie PCI virtio-gpu. Na maszynie bez niego (np. realny Dell — grafika
Intela, brak `1AF4:1050`) sam się wyłącza i używany jest framebuffer firmware, tak jak dotychczas
(patrz §5).

---

## 2. Framebuffer firmware (ścieżka domyślna)

Bootloader przekazuje liniowy framebuffer (adres, pitch, szerokość, wysokość, bpp) w boot info.
`bootinfo_x86_64.cpp` go parsuje; jeśli istnieje, `Kernel::start` stawia stos graficzny VT
(`VtManager` + `/dev/tty1..7`) oraz `/dev/fb0` (`Fb0Device`) bezpośrednio nad tą pamięcią. To ścieżka
na realnym Dellu (UEFI GOP) i przy zwykłym `make run64` (std-VGA w QEMU). 32-bpp BGRX. Żaden
sterownik nie jest zaangażowany — firmware już ustawił tryb.

---

## 3. Niezmodyfikowany sterownik virtio_gpu na LinuxKPI

Obraz napędza **niezmodyfikowany sterownik DRM `virtio_gpu` z Linux 6.12 (14 plików) + rdzeń DRM/KMS
(~59 plików) + rdzeń virtio**, skompilowany względem shimu **LinuxKPI** i zlinkowany w jeden
`virtio_gpu.nkext` bez zmian w źródłach upstream. LinuxKPI to reużywalna, niezależna od sterownika
warstwa — *czym jest, pełna powierzchnia KPI i uproszczenia kooperatywnego UP są opisane w
[linuxkpi.md](linuxkpi.md)*. Ta sekcja dotyczy tylko rzeczy **specyficznych dla virtio-gpu**.

```
                       virtio_gpu.nkext
   ┌───────────────────────────────────────────────────────────┐
   │  NIEZMODYFIKOWANE źródła Linux 6.12 (vendorowane w external/)│
   │   drivers/gpu/drm/virtio/*  (sterownik DRM virtio_gpu)      │
   │   drivers/gpu/drm/*         (rdzeń DRM/KMS, atomic, gem_shmem)│
   │   drivers/virtio/*          (virtio_ring, virtio_pci_modern)│
   ├───────────────────────────────────────────────────────────┤
   │  Shim LinuxKPI (linuxkpi/)  — linux/*.h + kpi_*.c           │
   │   slab, idr, dma-fence, scatterlist, dostawca stron shmem,  │
   │   workqueue (synchroniczny), printk (%pV/%ps), pci/mmio/dma │
   ├───────────────────────────────────────────────────────────┤
   │  Glue NanOS (kext/virtio_gpu/)                              │
   │   virtio_transport.c   nowoczesny transport virtio-pci      │
   │   virtio_gpu_drv_entry.c  bootstrap: rejestracja + probe    │
   │   virtio_gpu_present.c    scanout + most /dev/fb0           │
   └───────────────────────────────────────────────────────────┘
                 │ ABI knx_*            │ knx_fb_set_backing
                 ▼                      ▼
            jądro NanOS  ───────►  /dev/fb0  ◄──── tu rysują nwm / fbcon
```

### 3.1 Bring-up (`virtio_gpu_drv_entry.c`)

`loadAllKexts` uruchamia `nkext_init()` kexta przy starcie (przed schedulerem). Funkcja:

1. inicjalizuje rdzeń DRM (`drm_core_init`) i wyzwala własną rejestrację sterownika
   (niezmodyfikowane `module_virtio_driver()` rozwija się — przez makro shimu — w wywoływalny init);
2. szuka funkcji PCI virtio-gpu (`1AF4:1050`) — **jeśli jej nie ma, natychmiast wraca (no-op)**;
3. buduje dla niej `virtio_device` (ręcznie napisany nowoczesny transport, nasz odpowiednik
   `virtio_pci_common.c`) i odtwarza negocjację cech z `virtio_dev_probe`;
4. wywołuje **prawdziwe** `virtio_gpu_probe()` — które stawia urządzenie DRM, GEM, fence'y, connector
   (EDID → 1280×800) i KMS;
5. wywołuje `virtio_gpu_fbcon_bringup()`, żeby wyświetlić obraz.

### 3.2 Model przerwań (kooperatywny, INTx zamaskowane)

`loadAllKexts` działa **przed schedulerem**, jednowątkowo i bez wywłaszczania. INTx w virtio jest
**wyzwalane poziomem**: QEMU podnosi je przy pierwszym ukończonym buforze, a bez podpiętego handlera
linia zostaje wysoka i zalewa CPU w nieskończoność — blokując boot. Dlatego transport **maskuje INTx**
(`PCI_COMMAND.INTX_DISABLE`); ukończenia z virtqueue są zbierane przez **kooperatywny pump** — gdy kod
DRM/virtio czeka na fence lub odpowiedź z vq, oczekiwanie kręci się i woła `vt_interrupt()`, które
czyta rejestr ISR i uruchamia callback ringu. To wystarcza, by obsłużyć strumień komend GPU w czasie
bootu.

### 3.3 Scanout + most `/dev/fb0` (`virtio_gpu_present.c`)

`virtio_gpu_fbcon_bringup()` napędza scanout 0 w całości przez **własną warstwę komend sterownika**:

1. `virtio_gpu_object_create()` — sterownik alokuje bufor GEM (shmem) i wystawia prawdziwe
   `RESOURCE_CREATE_2D` + `RESOURCE_ATTACH_BACKING`;
2. `virtio_gpu_cmd_set_scanout()` — przypina ten zasób do scanoutu 0;
3. `knx_fb_set_backing()` — wystawia bufor jako `/dev/fb0` i rejestruje callback prezentacji;
4. co klatkę wątek prezentacji wystawia `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`, więc to, co
   narysowały fbcon / `nwm`, trafia na scanout.

Jeden kluczowy szczegół: **dostawca stron shmem w shimie podkłada pod każdy obiekt GEM jeden
fizycznie ciągły blok** (`linuxkpi/kpi_misc.c`), więc `page_address(pages[0])` jest płaskim
framebufferem współdzielonym przez CPU i urządzenie (RAM jest identity-mapped, więc ciągłość
wirtualna == ciągłość fizyczna; shim nie ma `vmap` zszywającego rozproszone strony).

Powierzchnię KPI, którą wciąga ten sterownik (slab, dma-fence, scatterlist, dostawca stron shmem,
synchroniczny workqueue, …) oraz kooperatywny model UP stojący za §3.2 opisuje ogólny shim — patrz
[linuxkpi.md](linuxkpi.md) §2–§3.

---

## 4. Dlaczego takie podejście

virtio-gpu wybrano jako pierwszy sterownik LinuxKPI, bo jego interfejs urządzenia to czysty protokół
virtqueue, a nie grzebanie w rejestrach konkretnego krzemu — więc ciężar leży na shimowaniu
reużywalnego **rdzenia DRM/KMS + virtio**, którego potrzebowałby też późniejszy `i915` / `iwlwifi`.
Uruchomienie sterownika *niezmodyfikowanego* (zamiast przepisywania go pod API NanOS) jest sednem:
dowodzi ścieżki shimu i trzyma się upstreamu.

---

## 5. Realny sprzęt: łagodny no-op

`virtio-gpu` to urządzenie wirtualne (QEMU). Realny Dell Latitude ma zintegrowaną grafikę Intela i
**nie ma** urządzenia `1AF4:1050`. Kext się ładuje, `knx_pci_find()` nic nie zwraca, a `nkext_init()`
zwraca `-1` **zanim** dotknie `/dev/fb0`. Pulpit renderuje się wtedy na framebufferze firmware (§2),
bez zmian. Obie ścieżki wyświetlania wykluczają się i są wybierane automatycznie po obecności
urządzenia — dodanie kexta virtio_gpu do obrazu nie wpływa na maszynę, która tego urządzenia nie ma.

---

## 6. Testy

- **Testy hostowe** (`make test64`) pokrywają prymitywy shimu w izolacji (slab, idr, scatterlist,
  sort, jiffies, DMA identity, warianty `%p` w printf). Nagłówki shimu są utrzymywane jako
  „host-clean” strażnikami `#ifndef NANOS_HOST_TEST`, żeby nie psuły libstdc++/glibc w buildzie
  doctest.
- **`smoke-virtio-gpu`** (`scripts/smoke-virtio-gpu.sh`, wpięte w `verify64`) startuje
  `-vga none -device virtio-gpu-pci` — virtio-gpu jako **jedyny** wyświetlacz, więc każdy piksel
  dowodzi, że narysował go sterownik — i sprawdza, że niezmodyfikowany sterownik robi probe, most
  scanout/fb0 wstaje, a pulpit `nwm` renderuje (klatka bogata w kolory) bez faultu jądra.

---

## 7. Granica GPL

Vendorowane źródła DRM/virtio Linuksa (GPLv2) kompilują się tylko do `virtio_gpu.nkext` i nigdy nie
są linkowane do `kernel.bin` — patrz [linuxkpi.md](linuxkpi.md) §5.

---

## 8. Pliki (specyficzne dla virtio-gpu)

Shim i vendorowane źródła wymienia [linuxkpi.md](linuxkpi.md) §8; glue specyficzne dla wyświetlania:

| Ścieżka | Co |
|---|---|
| `kext/virtio_gpu/virtio_transport.c` | nowoczesny transport virtio-pci; maskowanie INTx |
| `kext/virtio_gpu/virtio_gpu_drv_entry.c` | bootstrap: rejestracja + negocjacja cech + `virtio_gpu_probe()` |
| `kext/virtio_gpu/virtio_gpu_present.c` | konfiguracja scanoutu 0 + most `/dev/fb0` + prezentacja co klatkę |
| `kernel/KernelExports.cpp` | `knx_fb_set_backing` / `knx_fb_start_present` / `knx_boot_fb` |
| `scripts/smoke-virtio-gpu.sh` | gate wyświetlania w QEMU (w `verify64`) |

---

## 9. Pulpit GL (virgl, `nwm-gl`)

Obok ścieżki CPU nwm ma **kompozytor natywnie GPU** OpenGL-ES (`user/nwm/nw_compose_gl.c`, Plan 1
Zadanie 10). Komponuje pulpit na GPU: tapeta i zawartość każdego okna (cache `w->frame`) to tekstury
(swizzle `.bgr` dla pikseli `0x00RRGGBB` NanOS-a); każde szklane okno dostaje **prawdziwy dwuprzebiegowy
blur Gaussa na GPU** sceny pod spodem, który shader okna próbkuje pod maską zaokrąglonych rogów przy
alfa ciała okna. Chrome pulpitu (górny panel, pasek zadań, otwarte menu, modale Run/Auth) to jedyna
rzecz zostawiona na toolkicie 2D CPU — `nw_compose_chrome` renderuje je do przezroczystej nakładki
kluczowanej na czerni, rysowanej na końcu; kursor to kluczowany quad, a modalne przyciemnienie pulpitu
to quad GPU. Skomponowana klatka idzie na scanout kanoniczną ścieżką GPU Linuksa — GBM + kontekst EGL ES
(`user/glkms/glkms_init.c`), `eglSwapBuffers` → `drmModeSetCrtc` — bez odczytu przez CPU i bez blitu
`/dev/fb0`. Host-GPU (virglrenderer → ANGLE → Metal w QEMU; i915 na Dellu) rozwiązuje bufor na scanout.

**Reguła forka (blur bez zawiechy).** Fork kosmickrisp ANGLE→Metal zawiesza się na **przełączaniu
podpięcia FBO w środku klatki** — re-tworzenie/re-podpinanie render-targetu w klatce to granica
render-passu Metala, na której patch pożyczania tekstur się synchronizuje i wiesza (ta sama rodzina
kwirków co `glReadPixels`-zwraca-zero). Dlatego tekstury ping-pong blura **i** ich FBO są alokowane i
podpięte **raz przy init** (`g_blurA/g_fboA`, `g_blurB/g_fboB`, pełnoekranowe); każde okno robi blur do
narożnika `fw×fh` (sub-viewport) tych stałych celów — nigdy `glTexImage2D`/`glFramebufferTexture2D` w
klatce. Próbkowanie właśnie-wyrenderowanej tekstury w klatce jest OK; problemem było tylko przełączanie
podpięcia. Pokrętła: `NWM_NO_GLASS=1` wymusza okna nieprzezroczyste, `NWM_GL_TRACE=1` drukuje bracket
per-wywołanie w `blur_backdrop`; niekompletny FBO blura auto-spada do szkła nieprzezroczystego.

Wszystkie haki są pod `#ifdef NWM_GL`, więc in-tree `nwm.nxe` to czysty program CPU (bez zmian).
Wariant GL to **osobny** binarny, linkowany z Mesą: `make nwm-gl` (Docker, `build-nwm-gl.sh` w
`nanos-sdk-work/mesa-port`) → `nwm-gl.nxe`, instalowany przez `make image64-gl`. Runtime-fallback do
CPU przy `NWM_NO_GL=1`, braku węzła DRM (czyste QEMU) lub dowolnym błędzie GL/KMS. Klatka GL może
też polec *w trakcie sesji* (np. niepodpisany fence po złym prezencie); `nwm` traktuje to tak samo
jak awarię przy starcie — zwija kontekst GL (wedged, nie gracefully: kontekst, który właśnie oblał
klatkę, może trzymać fence'y, na których gracefully'owy `eglTerminate` czekałby w nieskończoność),
czyści `g_gl` i flagę keyed-frame, oznacza wszystkie okna jako brudne i wymusza jedno pełne
przemalowanie CPU. Od tego momentu pulpit do końca sesji jest klasycznym nieprzezroczystym
kompozytorem CPU — kluczowane ramki szkła są bez sensu bez shadera slabu GL, który by je konsumował.

### 9.1 Materiał szklanych okien (liquid glass)

Każde szklane okno (i chrome pulpitu wokół niego) rysuje jeden fragment shader, `FS_WIN` w
`nw_compose_gl.c`, jako pojedynczą przezroczystą "taflę" (slab), a nie płaski rozmyty prostokąt:

- **SDF zaokrąglonego prostokąta** (`sd_box`) daje odległość ze znakiem `d` do krawędzi okna. Pas ~14 px
  (`BEVEL`) tuż przy tej krawędzi to "pierścień soczewki", zbudowany z przepisu soczewki w stylu macOS
  (v3, §9.1.1 niżej): profil nachylenia po łuku koła, przesunięcie small-angle-Snell *do wewnątrz*
  (które próbkuje treść głębiej — czyli powiększa to, co jest przy krawędzi), aberracja chromatyczna i
  rozjaśnienie kaustyczne. Poza pierścieniem ciało okna pokazuje po prostu płaskie, rozmyte na GPU tło
  (mróz/frost), samo lekko wygięte, żeby przejście soczewka→mróz się nie ścinało. Fazowanie niesie też
  górno-lewy błysk specularny + obrzeże Fresnela oraz parę 1-pikselowych linii włosowych: ciemną
  zewnętrzną i białą wewnętrzną — dzięki temu tafla wygląda jak oświetlona, zakrzywiona szyba, a nie
  efekt malarski.
- Na zsoczewkowane/zmrożone ciało nakładany jest **wzmocnienie nasycenia + przekątny gradient tintu**
  (v3, §9.1.1): wyższe nasycenie i chłodny niebiesko-biały gradient przy fokusie, płaściej/bledziej bez
  fokusu, a dla okien `NW_STYLE_DARK` — prawie czarny gradient o większej gęstości; to właśnie daje
  Terminalowi jego ciemny, szklany wygląd. Na wierzchu jedzie przekątny połysk Aero (miękki biały blask
  w górnych ~40%).
- **Kule podpisów** (caption spheres) — żółta / zielona / czerwona szklana kulka, każda z własną
  mini-soczewką, punktem specularnym i kaustyką — są rysowane przez ten sam shader dokładnie nad
  klasycznymi slotami trafień close/maximize/minimize (czerwona w zewnętrznym rogu, jak przed
  wprowadzeniem szkła), więc hit-testing w logice okien `nwm` jest nietknięty: geometria, w którą
  celuje mysz, się nie zmieniła — zmieniło się tylko to, co się tam rysuje. Nieaktywne okna dostają
  szare kulki zamiast trójkolorowego zestawu.

**Dwa kontrakty atramentu (ink)** decydują, co liczy się jako nieprzezroczysty "atrament" nad taflą
szkła, oba liczone w tym samym shaderze:

1. **Atrament pasa ramki** (belka tytułu + obramowania, wszystkie okna): renderer CPU 2D czyści cały pas
   do w pełni przezroczystego płótna ARGB (`nw_compose_set_glass_frame(1)` w `nw_compose.c` +
   `nw_clear_argb`) i rysuje w nim tylko wyśrodkowaną poświatę tytułu Aero prawdziwymi prymitywami
   alfa (§9.1.2 niżej). Shader czyta tę alfę wprost — `ctex.a` — jako pokrycie atramentu i kompozytuje
   ją nad gotowym szkłem; to zastąpiło klucz luminancji z v1/v2 (`smoothstep` na najjaśniejszym kanale),
   który nie potrafił reprezentować prawie czarnego tekstu tytułu. Atrament pasa to teraz prawdziwa
   alfa, więc przetrwa dowolny kolor atramentu (także ciemny).
2. **Atrament klienta** dla okien `NW_STYLE_GLASS_CLIENT`: piksele klienta to `0xAARRGGBB` — górny bajt
   to prawdziwa alfa, kontrolowana przez aplikację per piksel — a shader robi `mix(glass, content,
   alpha)` wewnątrz prostokąta klienta. Okna klasyczne (styl 0, domyślny z `nw_create_window`)
   zachowują stare zachowanie: prostokąt klienta to nieprzezroczysty "wybite" okno wprost do
   `content`, szkło nigdy przez nie nie prześwituje. Słowo stylu podróżuje raz, przy tworzeniu, jako
   pole `c` w `NW_REQ_CREATE_WINDOW`; `nw_create_window_style(d, w, h, title, style)` w `libnw`
   je eksponuje (`NW_STYLE_GLASS_CLIENT = 1`, `NW_STYLE_DARK = 2`, łączalne bitowym OR;
   `nw_create_window` to po prostu styl 0). Terminal (`user/terminal/terminal.c`) jest tego
   pokazem: otwiera się z `NW_STYLE_GLASS_CLIENT | NW_STYLE_DARK`, maluje zwykłe komórki w pełni
   nieprzezroczyście (pełny atrament), a domyślny kolor tła (indeks komórki 0) tylko cienką woalką
   alfa `0x50`, więc ciemna tafla szkła prześwituje za nienapisaną przestrzenią terminala.

`NW_BORDER` wynosi 6 px (wcześniej 2 px, przed szkłem) — wystarczająco, by pierścień soczewki fazowania
czytał się jako odrębny pas wokół klasycznej szerokości ramki.

#### 9.1.1 Przepis soczewki krawędzi + przebieg cienia (v3)

Rim w `FS_WIN` przeszedł przez v2 (stylizowane przesunięcie kwadratowe), a potem przepisanie w v3 na
konsensusowy przepis "soczewki macOS" (stałe na górze `FS_WIN` w `nw_compose_gl.c`):

- `x = clamp(1.0 + d/BEVEL, 0, 1)` — 0 głęboko wewnątrz tafli, 1 dokładnie na krawędzi.
- `s = x / sqrt(max(1 - x*x, 0.0625))` — nachylenie profilu po łuku koła, przycięte od dołu, żeby
  piksel na krawędzi nie eksplodował w tęczowy szum (maks. nachylenie 4.0).
- `bend = s * (1 - 1/IOR) * thick` px — przesunięcie small-angle-Snell wzdłuż wewnętrznego gradientu
  SDF (`IOR = 1.50`); `thick` (14–26 px) skaluje się z mniejszym wymiarem okna oraz z `u_focus` (szkło
  bez fokusu jest cieńsze/łagodniejsze). Próbkowanie *do wewnątrz* o `bend` px to właśnie to, co
  powiększa treść blisko krawędzi (odpowiada "treść przy krawędzi wygląda na wypchniętą na zewnątrz").
- `ca = g * (s * CA_PX)` (`CA_PX = 2.5`) przesuwa próbki R i B w przeciwnych kierunkach względem próbki
  G — obrzeże aberracji chromatycznej.
- `glass *= 1.0 + CAUSTIC * s * 0.25` (`CAUSTIC = 0.25`) rozjaśnia krawędź proporcjonalnie do
  nachylenia — kaustyka "światło koncentruje się na krawędzi".
- Samo mieszanie soczewka/mróz to `lens = smoothstep(0.0, 0.7, x)`, `mix(body, ring, lens)`, gdzie
  `body` (płaski mróz) też próbkuje z tłumionym `bend * 0.35`, żeby granica soczewka→mróz się nie ścinała.
- Pseudo-3D normalna `N = normalize(vec3(g*s, 1))` względem stałego kierunku światła (`LIGHT`) napędza
  błysk specularny; `fres = pow(x, 2.5) * 0.4` to rozjaśnienie krawędzi w stylu Fresnela (Sorrell).

**Materiał tafli** (ten sam shader, zaraz po soczewce): nasycenie `mix(mix(1.0, 1.65, u_focus), 1.35,
u_dark)` na kolorze po soczewkowaniu; trzy-stopniowy przekątny gradient tintu (fokus/brak
fokusu/ciemny mają własne kolory+alfy stopni, przepisane z CSS makiety); przekątny połysk Aero
(`rgba(255,255,255,.34→.10→0)`, o połowę mniejszy bez fokusu, ~0,41× dla ciemnego); potem linie
włosowe zewnętrzna/wewnętrzna (`rgba(8,16,30,.55)` zewnętrzna, biała `.62` wewnętrzna, wzmocniona do
`~.85` na samej górnej krawędzi).

**Cienie okien (drop shadows)** to osobny program (`FS_SHADOW`), rysowany jako jeden powiększony quad
*przed* quadem `FS_WIN` każdego szklanego okna (`nw_gl_frame`, bramkowane na `glass`): analityczny,
miękki SDF-owy prostokąt, dopełniony `SH_PAD = 36` px poza ramką z każdej strony i przesunięty
`SH_OFFY = 10` px w dół, zanikający na `20–30` px (głębiej/szerzej przy fokusie) do szczytowej alfy
`0,60` przy fokusie / `0,42` bez fokusu, kolor `rgba(4,10,24)` — dwuwarstwowy cień z makiety zwinięty
w jeden miękki analityczny zanik. Bez nowych tekstur/FBO (reguła forka): to program tylko-rysujący,
współdzielący `VS_QUAD`.

#### 9.1.2 Poświata tytułu + automatyczna polaryzacja atramentu

v1 rysowała pasek tytułu jako 8-kopiową poświatę ±1px; v3 zastępuje ją prawdziwą poświatą Aero, wciąż
renderowaną na CPU, ale teraz prawdziwą-alfą (`draw_caption_glow` w `nw_compose.c`):

1. Zrasteryzuj pokrycie glifów tytułu raz do lokalnego bufora bajtowego (`rasterize_run_coverage`, ten
   sam przebieg glifów co `nw_text`/`nw_text_argb`, ze świadomością fallbacku VGA).
2. Rozmyj to box-blurem dwukrotnie (running-sum, promień `GLOW_R = 6`, potem `GLOW_R/2`) — tani
   ~Gauss.
3. Skomponuj najpierw rozmytą "kartkę" (gain `4` przy fokusie / `2` bez fokusu, alfa z limitem) w
   jasnym lub ciemnym kolorze poświaty, potem ostry rdzeń po remapie `cov143` na wierzchu (przyciemniony
   do `78%` alfy bez fokusu) — oba przez `nw_over_pixel` (prawdziwa alfa), więc miękkie obrzeże poświaty
   przetrwa aż do odczytu `ctex.a` w shaderze GL (§9.1 wyżej). Tytuły dłuższe niż `GLOW_MAXW - 4*GLOW_R`
   (~470 px) są przycinane do statycznego bufora poświaty.
4. **Polaryzacja** — jasna poświata + ciemny rdzeń dla jasnego okna, albo ciemna poświata + jasny rdzeń
   dla ciemnego — to wariant samego okna, nie coś próbkowanego z tła: `draw_window_to` ustawia wprost
   `dark_ink = !dark` (`dark` to ta sama flaga `NW_STYLE_DARK`, która już wybiera kolory materiału/paska
   tytułu okna). Był tu kiedyś próbnik luminancji tapety (rzadka próbka 8×2 z pasmem histerezy);
   usunięto go jako martwy kod — jasna tafla szkła podnosi to, co jest pod nią,
   o mniej więcej 55% w stronę bieli, zanim wyląduje na niej atrament, więc ciemny atrament + biała
   poświata czyta się nad *każdą* tapetą, na jakiej może stać jasne okno (semantyka Win7 Aero i to, co
   pokazuje makieta). Okna `NW_STYLE_DARK` (Terminal) bezwarunkowo dostają jasny rdzeń.

#### 9.1.3 Jasno-szklane wnętrza (libnwui)

`NW_STYLE_GLASS_CLIENT` rozciąga się poza pas ramki w obszar klienta: `nwui_open_style(title, w, h,
NW_STYLE_GLASS_CLIENT)` (libnwui) otwiera okno, którego toolkit maluje się w **trybie szklanym** zamiast
klasycznego nieprzezroczystego tła papieru — `nwui_open(title, w, h)` to po prostu
`nwui_open_style(..., 0)`.

- Okno czyści się do w pełni przezroczystego płótna ARGB (`nw_clear_argb`, nie `nw_fill_rect`), a każdy
  widget maluje przez rodzinę prymitywów prawdziwej-alfy dodaną w tym celu (`user/libnw/nw_gfx.{c,h}`):
  `nw_clear_argb`, `nw_over_pixel/rect/round` (src-over, które też akumuluje alfę celu), `nw_over_ring`
  (ta sama geometria AA zaokrąglonego prostokąta co `nw_over_round`, ale zachowująca tylko pas
  zewnętrzny-minus-wewnętrzny o szerokości 1px — prawdziwy obrys prawdziwej-alfy) oraz `nw_text_argb`
  (glify przez ten sam remap gamma `cov143` co klasyczny tekst, skalowane przez alfę koloru atramentu).
  Dowolne wywołanie maskowane-RGB (`nw_fill_*`, `nw_blend_*`, `nw_draw_char_t`)
  zawsze zapisuje alfę 0, co pod shaderem GL czyniłoby ten piksel niewidzialnym — więc w trybie
  szklanym każde malowanie w obrębie widgetu idzie ścieżką ARGB, nie tylko te, które dostają nowy kolor.
- Paleta (`nwui_paint.c`, prawdziwe ARGB) mapuje język atramentu/scrimów makiety na toolkit: `GCOL_INK`
  (`#17222f`) / `GCOL_INK_SOFT` (to samo przy 62%) dla tekstu, `GCOL_SCRIM` (biel 25%) dla ciał paneli,
  `GCOL_FIELD` (biel `0x30`, ~19%) dla studni pól/list/textarea, `GCOL_SEL` (biel `0x59`, 35%) /
  `GCOL_SEL_RING` (biel `0x80`, 50%) dla pigułki zaznaczenia + jej wcięcia, oraz `GCOL_BTN_TOP/BOT/RING`
  dla pigułek przycisków. To dosłowne wartości alfa CSS z makiety, bez zmian: `GCOL_FIELD` jest celowo
  *niska* (obniżona, nie podniesiona, względem wcześniejszej wersji roboczej), żeby zawartość studni
  malowała się wprost na surowej tafli szkła, zgodnie z wyglądem makiety — "ledwie ślad definicji, nie
  biały panel"; mocniejsze wypełnienie czytałoby się jako nieprzezroczysta karta unosząca się nad
  szkłem, nie jak studnia wycięta w nim. `GCOL_SEL`/`GCOL_SEL_RING` też stosują się dosłownie, bo
  `glass_ring` (niżej) nie składa już pierścienia pod wypełnieniem wnętrza — każdy z nich to dokładnie
  jeden przebieg malowania, więc alfa z makiety jest dokładnie tym, co ląduje na ekranie.
- `glass_ring(s, x, y, w, h, r, ring, fill)` udaje przezroczysty 1px obrys (nie ma prawdziwej-alfy
  odpowiednika `nw_stroke_round`) jako **jedną warstwę wypełnienia, nie dwie zachodzące na siebie**:
  wypełnia CAŁY obszar kolorem `fill` (`nw_over_round`), potem maluje krawędź jako prawdziwy 1px
  wygładzony pas przez prymityw prawdziwej-alfy `nw_over_ring` (`user/libnw/nw_gfx.{c,h}`) — obrys
  src-over, który próbkuje zewnętrzny i wewnętrzny łuk pokrycia zaokrąglonego prostokąta i zachowuje
  tylko ich różnicę. Ponieważ pierścień jest własnym, izolowanym pasem, a nie drugim pełnym wypełnieniem
  nałożonym na pierwsze, może być *jaśniejszy* od wnętrza, które otacza (wcięta linia włosowa z makiety),
  bez żadnego składania pierścień-na-wypełnieniu do skorygowania — alfy `ring` i `fill` można dostrajać
  niezależnie.
- Kontenery ogólne (row/column/box) z tłem ustawionym przez aplikację (`n->has_bg`, np.
  `.colors(fg, bg)`) miały lukę w mapowaniu: tryb szklany wymuszał alfę 255 na `n->bg`, więc każdy panel
  autorstwa aplikacji (pasek boczny Plików, pasek stanu) renderował się w pełni nieprzezroczyście,
  łamiąc ciągłość tafli szkła. Naprawiony kontrakt (przypadek domyślny w `nwui_paint.c`): kolor
  **legacy** `0xRRGGBB` (górny bajt 0 — zwykłe, sprzed-szkła wywołanie `.colors()`) kompozytuje się jako
  przezroczysty scrim przy alfie `0x59`, zgodnie z ogólną alfą pól/paneli; kolor z **niezerowym** górnym
  bajtem jest honorowany dosłownie — aplikacja dokonała jawnego wyboru alfy. Okna legacy (nie-szklane)
  są nietknięte w obu przypadkach.
- Ikony PNG w stylu `NWUI_ICON_KEY` mieszają swoją prawdziwą alfę per-piksel przez `nw_over_pixel` w
  trybie szklanym (`nw_blend_pixel` w legacy) — alfa ikon nigdy nie przechodzi przez LUT glifów
  `cov143`.
- Fallback CPU (`NWM_NO_GL=1`): klasyczny kompozytor ignoruje górny bajt alfy, więc płótno aplikacji w
  stylu szklanym pokazuje się na czarno wszędzie, gdzie malowała ścieżką ARGB (przezroczystość →
  zapisana jako `0x000000`); każdy piksel atramentu wciąż niesie własne nieprzezroczyste RGB, więc
  tekst/ikony pozostają w pełni czytelne na tym czarnym płótnie. Akceptowane ograniczenie kosmetyczne
  fallbacku, nie błąd.

#### 9.1.4 Ciemno-szklane pasy menu i zadań

Górny pasek i pasek zadań chrome pulpitu dostają ten sam ciemno-szklany materiał co okno
`NW_STYLE_DARK`, przez trzeci program, `FS_BAR` (płaski mróz, bez fazowania/soczewki — paski są na tyle
cienkie, że ostra próbka + mocny tint + nasycenie czyta się jako mrożone szkło bez prawdziwego przebiegu
blura): nasycenie `1,35`, tint w stronę `rgba(12,17,28)` przy gęstości `,42`, plus 1px linia włosowa na
tej krawędzi, która graniczy z pulpitem (`u_topline` wybiera górę vs. dół). Oba quady pasów są rysowane w
`nw_gl_frame` *po* oknach/przyciemnieniu modalnym i *przed* nałożeniem kluczowanej nakładki chrome CPU —
więc okna widocznie wsuwają się **pod** paski, a atrament chrome (jasne kolory w trybie szklanym:
`NW_GLASS_INK_FG/MUT/FILL` w `nw_compose.c`, dobrane tak, by jednoznacznie przechodziły odrzucenie
`FS_KEYED` przy ~5/255 od czerni) siedzi na nich na wierzchu. `FS_BAR` próbkuje migawkę
`glCopyTexSubImage2D` właśnie skomponowanej sceny we własnym prostokącie ekranowym paska (ponownie
używając `g_grab`, nigdy nie podpiętego jako FBO), a nie `g_scene_tex` wprost — próbkowanie tekstury,
która jest właśnie podpiętym załącznikiem aktywnego FBO, jest niezdefiniowane, a FBO sceny jest wciąż
podpięte w tym momencie klatki.

**Pokrętła debug/ucieczki**, poza `NWM_NO_GL`/`NWM_NO_GLASS` opisanymi wyżej:

- `NWM_GLASS_DEBUG=1..4` podmienia finalny kolor tafli na diagnostykę: `1` = profil soczewki `x`
  (kanał czerwony, 0 głęboko wewnątrz → 1 na krawędzi), `2` = wielkość `bend` Snella znormalizowana do
  jej teoretycznego maksimum (`1,33 * 26,0` px), `3` = ostry chwyt tła bez soczewkowania, `4` = rozmyty
  chwyt tła. Przydatne do izolowania, czy glitch wizualny leży w matematyce SDF/soczewki, w
  przesunięciu, czy w samych teksturach chwytu/rozmycia.

Gate: `scripts/smoke-virtio-gpu-gl.sh` (`make smoke-virtio-gpu-gl`) — bramka developerska (wymaga
fork-QEMU virgl **i** GUI cocoa; scanout `gl=es`/ANGLE→Metal nie ma ścieżki headless), SKIPuje bez
forka, celowo poza headless `verify64`.

| Ścieżka | Co |
|---|---|
| `user/nwm/nw_compose_gl.{c,h}` | natywnie GPU kompozytor GL ES nwm (okna + blur szkła na GPU + nakładka chrome CPU); `FS_WIN`/`FS_SHADOW`/`FS_BAR` |
| `user/nwm/nw_compose.c` | chrome CPU (panel/pasek zadań), poświata tytułu Aero (polaryzacja = wariant jasny/ciemny samego okna) |
| `user/libnw/nw_gfx.{c,h}`, `user/libnw/nw_over_core.h` | LUT gamma `nw_cov143` + rodzina prymitywów ARGB prawdziwej alfy |
| `user/libnwui/nwui.{c,h}`, `user/libnwui/nwui_paint.c` | `nwui_open_style` + malowarki/paleta widgetów w trybie szklanym |
| `user/glkms/glkms_init.{c,h}` | wspólna sekwencja GBM+EGL+KMS (też oracle glkms) |
| `scripts/smoke-virtio-gpu-gl.sh` | gate pulpitu GL (developerski, SKIPuje bez forka) |
