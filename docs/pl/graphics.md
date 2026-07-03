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
CPU przy `NWM_NO_GL=1`, braku węzła DRM (czyste QEMU) lub dowolnym błędzie GL/KMS.

Gate: `scripts/smoke-virtio-gpu-gl.sh` (`make smoke-virtio-gpu-gl`) — bramka developerska (wymaga
fork-QEMU virgl **i** GUI cocoa; scanout `gl=es`/ANGLE→Metal nie ma ścieżki headless), SKIPuje bez
forka, celowo poza headless `verify64`.

| Ścieżka | Co |
|---|---|
| `user/nwm/nw_compose_gl.{c,h}` | natywnie GPU kompozytor GL ES nwm (okna + blur szkła na GPU + nakładka chrome CPU) |
| `user/glkms/glkms_init.{c,h}` | wspólna sekwencja GBM+EGL+KMS (też oracle glkms) |
| `scripts/smoke-virtio-gpu-gl.sh` | gate pulpitu GL (developerski, SKIPuje bez forka) |
