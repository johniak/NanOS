# Magistrala PCI w NanOS

Warstwa PCI enumeruje magistralę i dekoduje rejestry adresów bazowych (BAR), żeby sterownik mógł
znaleźć swoje urządzenie, zmapować jego rejestry i włączyć bus mastering. To hook do wykrywania, na
którym wiesza się każdy sterownik PCI — dziś kext karty sieciowej e1000 (@docs/kext.md,
@docs/networking.md §2.1). Rdzeń jest machine-independent i host-tested; tylko sam mechanizm dostępu
do config space jest zależny od architektury.

---

## 1. Podział MI/MD

- **`kernel/Pci.*` (MI)** — enumeracja + dekodowanie BAR nad **wstrzykiwanym backendem** config
  space (dwa 32-bitowe akcesory). Brak port I/O, więc cały moduł jest host-tested na zamockowanym
  config space (`tests/test_pci.cpp`).
- **`<arch/pci.h>` (kontrakt)** — `pciConfigRead32`/`pciConfigWrite32(bus, dev, func, off)`.
- **`arch/x86/io/pci_x86.cpp` (MD)** — x86 „configuration mechanism #1": zapisz adres na port
  `0xCF8`, czytaj/pisz dword na `0xCFC`. Inna architektura zaimplementowałaby te same dwie funkcje
  nad oknem MMIO ECAM.

Config space jest naturalnie **adresowane dwordami**; akcesory 8/16-bitowe wyprowadza się w rdzeniu
MI przez maskowanie / read-modify-write, więc powierzchnia MD to tylko te dwie funkcje.
`Pci::setBackend(rd, wr)` instaluje je przy starcie (kernel podaje `arch::pciConfigRead32/Write32`;
testy podają mock).

---

## 2. Zdekodowane urządzenie + BAR-y

`probe(bus, dev, func, out)` czyta slot i wypełnia `PciDevice` (true wtw. `vendor != 0xFFFF`):

```c
struct PciDevice {
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint8_t  classCode, subclass, progIf, revision;
    uint8_t  headerType;        // low 7 bits = layout; bit7 = multifunction
    uint8_t  irqLine, irqPin;
    PciBar   bar[6];
};
struct PciBar { uint32_t addr, size; bool isIo, is64, prefetch; };
```

**Sizing BAR-a** (`readBars`) to standardowa sztuczka write-all-ones / read-back: zapamiętaj BAR,
zapisz `0xFFFFFFFF`, odczytaj z powrotem, zamaskuj bity typu i `size = ~masked + 1` (potęga dwójki).
I/O vs pamięć, 64-bit (jego wyższy dword zajmuje następny slot, który jest wtedy zerowany) oraz
prefetchable dekoduje się z dolnych bitów flag.

---

## 3. API (`kernel/Pci.h`)

| Grupa | Funkcje |
|---|---|
| Backend | `setBackend(rd, wr)`, `hasBackend()` |
| Surowy config | `read8/16/32`, `write8/16/32(bus, dev, func, off)` |
| Wykrywanie | `probe(slot, out)`, `enumerate(out, max)` (brute-force skan → liczba), `find(vendor, device, out)` (`0xFFFF` = wildcard) |
| Bity command (RMW) | `enableBusMaster(d)`, `enableMemSpace(d)`, `enableIoSpace(d)` |

Standardowe offsety są w nagłówku (`PCI_VENDOR_ID … PCI_BAR0 … PCI_IRQ_LINE`), wraz z
`PCI_CMD_IO/MEM/MASTER` i `PCI_VENDOR_NONE = 0xFFFF`.

---

## 4. Podpięcie przy starcie i użycie przez sterownik

`Kernel::start` (@docs/boot.md §4) instaluje backend i skanuje:

```cpp
Pci::setBackend(arch::pciConfigRead32, arch::pciConfigWrite32);
// pciScanReport(): enumerate() magistrali, log liczby, find(0x8086, 0x100E) → e1000
```

Dzieje się to **przed** załadowaniem kextów, bo sterownik NIC znajduje swoje urządzenie przez PCI.
Kext sterownika nie woła `kernel::Pci` bezpośrednio — idzie przez stabilne eksporty `knx_pci_*`
(@docs/kext.md §4), które opakowują te funkcje: `knx_pci_find` → `Pci::find`, `knx_pci_bar`/
`knx_pci_bar_size`/`knx_pci_bar_is_io` → zdekodowany `PciBar`, `knx_pci_irq` → `irqLine`,
`knx_pci_enable_bus_master` → `Pci::enableBusMaster`, plus surowe `knx_pci_cfg_read32/write32`.
Dodanie sterownika PCI to więc: `find` urządzenia, odczyt BAR-a, `knx_map_mmio`, włączenie bus
mastering, podpięcie IRQ (@docs/networking.md §2.1 — e1000 robi dokładnie to).

> Kontrast z Linuksem: brak modelu sterowników PCI / hotplug / MSI-X / przechodzenia capability /
> zarządzania energią — płaski brute-force skan i dekodowanie BAR, tyle ile trzeba do podpięcia
> jednej karty.

**Kluczowe pliki:** `kernel/Pci.{h,cpp}`, `arch/include/arch/pci.h`, `arch/x86/io/pci_x86.cpp`,
`kernel/Kernel.cpp` (skan), `tests/test_pci.cpp`.
