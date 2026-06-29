# LinuxKPI — uruchamianie niezmodyfikowanych sterowników Linux na NanOS

**LinuxKPI** to shim *zgodności źródłowej* w przestrzeni jądra: drzewo nagłówków `linux/*.h` plus
implementacje `kpi_*.c`, które mapują **liściowe** API jądra Linux na prymitywy NanOS, tak by
**niezmodyfikowany sterownik Linux** (i rdzeń podsystemu, od którego zależy) dał się przekompilować i
uruchomić jako `.nkext` NanOS. To **Stream H — zgodność ze sterownikami Linux** z mapy drogowej.

Ten dokument opisuje **reużywalną** warstwę shimu — część, która *nie* jest specyficzna dla żadnego
sterownika. Pierwszym konsumentem jest sterownik GPU `virtio_gpu` ([graphics.md](graphics.md)); ta
sama powierzchnia jest fundamentem, na którym oparłby się późniejszy `i915` / `iwlwifi`.

---

## 1. Idea (model `drm-kmod`)

Nie przepisuj sterowników Linuksa pod API NanOS — to niszczy sens (reużycie, trzymanie się upstreamu)
i się nie generalizuje. Zamiast tego:

- **Kompiluj rdzeń podsystemu z vendorowanych źródeł Linuksa**, niezmodyfikowany (DRM/KMS, virtio, …).
- **Shimuj tylko liściowe API**, które ten rdzeń wywołuje (pamięć, PCI, MMIO, DMA, synchronizacja, id, printk).
- Odkrywaj dokładny zbiór symboli **sterowany błędami linkera**: zlinkuj wszystkie obiekty, wypisz
  niezdefiniowane symbole, zaimplementuj brakujący liść, powtarzaj aż się zlinkuje.

Dokładnie tak FreeBSD uruchamia linuksowy `drm-kmod`. Shim to *pragmatyczne tłumaczenie*, nie wierne
jądro (patrz §3).

```
                       <sterownik>.nkext
   ┌───────────────────────────────────────────────────────────┐
   │  NIEZMODYFIKOWANE źródła Linuksa (vendorowane w external/)  │
   │   sterownik + potrzebny rdzeń podsystemu (DRM, virtio, …)   │
   ├───────────────────────────────────────────────────────────┤
   │  Shim LinuxKPI (linuxkpi/)  —  linux/*.h  +  kpi_*.c         │
   │   liściowe API przetłumaczone na ABI kext knx_*             │
   ├───────────────────────────────────────────────────────────┤
   │  Glue NanOS (kext/<sterownik>/)                             │
   │   transport / bootstrap / most świadomy NanOS               │
   └───────────────────────────────────────────────────────────┘
                          │ ABI knx_*
                          ▼
                     jądro NanOS
```

---

## 2. Powierzchnia KPI (Linux API → backing NanOS)

Shimowana powierzchnia, wg kategorii. (Odkrywana sterowana błędami linkera; to, co realnie wciągnął
stos DRM/virtio.)

| Kategoria | API Linux | Backing NanOS (`linuxkpi/`) |
|---|---|---|
| slab | `kmalloc/kzalloc/kfree/krealloc`, `kmem_cache_*`, `vmalloc`, `__get_free_pages` | `kpi_slab.c` na `knx_malloc`/`knx_free` (ciągły heap identity-mapped; nagłówek rozmiaru dla `ksize`/`krealloc`); `kmem_cache`/`vmalloc` = cienkie wrappery |
| strony/shmem | `alloc_pages_exact`, `shmem_file_setup`, `shmem_read_folio_gfp` | `kpi_mm.c` + `kpi_misc.c`: każde mapowanie shmem = jeden **ciągły** blok pocięty na folio stron (`page == wirtualny-jądra == fizyczny`) — backing w stylu gem_shmem |
| DMA | `dma_alloc_coherent`, `dma_map_sg/sgtable`, `dma_set_mask_and_coherent` | `kpi_dma.c`: brak IOMMU → `dma_addr = phys = virt`; `dma_map_sgtable` wpisuje `dma_address = sg_phys` |
| PCI / MMIO | `pci_enable_device/set_master`, `pci_iomap`, `readl/writel`, `ioread/iowrite` | `kpi_pci.c` + `io.h` na `knx_pci_*` / `knx_map_mmio` + volatile deref |
| scatterlist | `sg_alloc_table`, `sg_alloc_table_from_pages_segment`, `for_each_sg` | `kpi_sg.c` (skleja ciągłe strony identity-mapped) |
| synchronizacja | `spinlock_t`, `mutex`, `ww_mutex`, `completion`, `wait_event*`, `atomic_t`, `set_bit` | `spinlock.h`/`kpi_fence.c`: **kooperatywny model UP** — locki to no-opy, `wait_event*` kręci się i pompuje (patrz §3); `atomic`/bitops = wbudowane `__atomic` |
| dma-fence / resv | `dma_fence_*`, `dma_resv_*`, `dma_buf_*` | `kpi_fence.c`: wierne kref + lista callbacków + signal; kooperatywne czekanie; minimalne resv/dma_buf (wzorzec `drm-kmod`, bez liftowania `dma-buf/*.c`) |
| kontenery/id | `kref`, `idr/ida`, `rbtree` (augmented) | `kpi_idr.c` (płaska tablica idr/ida); prawdziwe `lib/rbtree.c` zliftowane |
| workqueue | `INIT_WORK`, `schedule_work/queue_work`, delayed work | `workqueue.h`: **synchronicznie** — `schedule_work(w)` woła `w->func(w)` w miejscu (patrz §3) |
| timery/jiffies | `jiffies`, `msleep`, `udelay`, `time_after` | `kpi_time.c`: jiffies z `knx_uptime_us` @ HZ=1000 |
| printk/misc | `printk/pr_*/dev_*`, `sort/bsearch`, `seq_file`, `sysfs_emit` | `kpi_print.c` (własny `vsnprintf` z `%pV`/`%ps`/`%pe` → `knx_log`), `kpi_sort.c`, stuby w `kpi_misc.c` |
| device/bus | `struct device`, `dev_set/get_drvdata`, `module_*` | minimalna struktura + `void* drvdata`; **dopasowanie po szynie pominięte** — glue ręcznie buduje urządzenie i woła `probe()` sterownika bezpośrednio |

---

## 3. Świadome uproszczenia (kooperatywny model UP)

Shim wystarcza dla jednoproducenckiego urządzenia stawianego przy starcie i jest znacznie mniejszy
niż wierne jądro. Trzy wybory są istotne i różnią się od mainline:

- **Workqueue jest synchroniczny.** `schedule_work()` woła funkcję pracy w miejscu; nie ma
  asynchronicznego wątku. Callback ukończenia urządzenia od razu uruchamia swoje zdejmowanie.
- **Locki to no-opy w trakcie bring-upu.** Moduły ładują się przed schedulerem, jednowątkowo i bez
  wywłaszczania, a kooperatywny pump może *ponownie wejść* w sterownik przy trzymanym locku;
  prawdziwy lock by się zakleszczył (podobnie zagnieżdżone locki podsystemu). No-op to tu poprawny
  model UP.
- **Brak IRQ urządzenia; zamiast tego kooperatywne odpytywanie.** Przerwanie urządzenia jest
  zamaskowane; `wait_event*` / `dma_fence_wait` kręcą się i wołają hak pompujący, który obsługuje
  urządzenie, z **ograniczonymi** timeoutami, żeby utracone ukończenie nie zawiesiło bootu.

Wierne RCU / MM / wątkowy-workqueue / realne-IRQ to praca na przyszłość. Dopóki sterownik nie
potrzebuje prawdziwej współbieżności, kooperatywny model to właściwa ilość shimu.

---

## 4. Zliftowane vs shimowane

- **Zliftowane (kompilowane z vendorowanych źródeł, niezmodyfikowane):** *rdzeń* podsystemu i
  sterownik — np. rdzeń DRM/KMS, `drm_gem_shmem_helper`, rdzeń virtio (`virtio_ring`,
  `virtio_pci_modern`), `lib/rbtree.c` i `drivers/gpu/drm/virtio/*`.
- **Shimowane (nasze `linuxkpi/`):** liściowe API z §2.
- **Ani jedno, ani drugie (zastąpione glue NanOS):** instalacja dopasowania po szynie / modelu
  urządzeń i krawędzie zwrócone ku userspace (emulacja fbdev, debugfs/sysfs, render nodes) — glue
  ręcznie buduje urządzenie i podpina wynik do NanOS.

---

## 5. Build i granica GPL

- `scripts/vendor-linux.sh` wyciąga dokładny zestaw potrzebnych plików Linuksa do `external/linux-6.12/`.
- Makefile kompiluje je z `LINUXKPI_CFLAGS` + vendorowanym drzewem include, linkuje zliftowane
  obiekty + shim + glue w jeden `.nkext` (`mknx64`), rozwiązując względem eksportów `knx_*`.
- **Granica GPL:** vendorowane źródła Linuksa (GPLv2) leżą wyłącznie pod `external/`, kompilują się
  tylko do `.nkext` sterownika i **nigdy nie są linkowane do `kernel.bin`** (który zostaje na licencji
  typu MIT). Granicą są shim (`linuxkpi/`) i glue (`kext/<sterownik>/`).

---

## 6. Testy hostowe

Prymitywy shimu są testowane jednostkowo na hoście (`make test64`, doctest): slab + rozliczanie,
`idr/ida`, scatterlist, `sort/bsearch`, arytmetyka jiffies, DMA identity, warianty `%p` w printf.
Nagłówki shimu są utrzymywane jako **host-clean** strażnikami `#ifndef NANOS_HOST_TEST`, żeby nie
psuły libstdc++/glibc w buildzie doctest (makra w stylu jądra `min`/`max`/`static_assert`/`INT_MAX`,
typedefy `pid_t`/`dev_t`, deklaracje stringów zwracające `char*` itd. emitowane są tylko na ścieżce
kext). Pokrycie jest bramkowane na ≥90%.

---

## 7. Dodanie kolejnego sterownika Linux

Powyższa powierzchnia jest reużywalna; nowy sterownik to z grubsza:

1. `vendor-linux.sh`: dodaj sterownik + potrzebny rdzeń podsystemu.
2. Zbuduj; dla każdego niezdefiniowanego liścia dodaj go do shimu (większość §2 już jest).
3. Napisz glue NanOS: transport (jeśli szyna nie jest jeszcze shimowana), bootstrap, który ręcznie
   buduje urządzenie i woła `probe()` sterownika, oraz most wystawiający wynik jako urządzenie NanOS.
4. Dodaj testy hostowe dla nowych prymitywów shimu; dodaj gate smoke w QEMU.

Uproszczenia kooperatywnego UP (§3) obowiązują dla urządzenia stawianego przy starcie i
jednoproducenckiego; sterownik wymagający prawdziwej współbieżności najpierw popchnąłby shim ku
wątkowym workqueue + realnym IRQ.

---

## 8. Pliki

| Ścieżka | Co |
|---|---|
| `linuxkpi/include/linux/*.h` | nagłówki kompatybilności `linux/*` |
| `linuxkpi/kpi_*.c` | implementacje liściowych API (slab, idr, dma, fence, sg, print, sort, time, misc, …) |
| `external/linux-6.12/` | vendorowane, niezmodyfikowane źródła Linuksa (odseparowane GPL) |
| `scripts/vendor-linux.sh` | wyciąga potrzebne pliki Linuksa |
| `kext/<sterownik>/` | glue NanOS dla danego sterownika |

Pierwszą realizacją jest sterownik wyświetlania virtio-gpu — patrz [graphics.md](graphics.md).
