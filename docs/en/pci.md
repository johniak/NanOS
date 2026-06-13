# NanOS PCI Bus

The PCI layer enumerates the bus and decodes Base Address Registers (BARs) so a driver can find
its device, map its registers, and turn on bus mastering. It is the discovery hook every PCI driver
hangs off — today the e1000 NIC kext (@docs/kext.md, @docs/networking.md §2.1). The core is
machine-independent and host-tested; only the config-space access mechanism is arch-specific.

---

## 1. MI/MD split

- **`kernel/Pci.*` (MI)** — enumeration + BAR decode over an **injectable config-space backend**
  (two 32-bit accessors). No port I/O, so the whole module is host-tested against a mock config
  space (`tests/test_pci.cpp`).
- **`<arch/pci.h>` (contract)** — `pciConfigRead32`/`pciConfigWrite32(bus, dev, func, off)`.
- **`arch/x86/io/pci_x86.cpp` (MD)** — x86 "configuration mechanism #1": write the address to port
  `0xCF8`, read/write the dword at `0xCFC`. Another arch would implement the same two functions over
  an ECAM MMIO window.

Config space is naturally **dword-addressed**; the 8/16-bit accessors are derived in the MI core by
masking / read-modify-write, so the MD surface is just those two functions. `Pci::setBackend(rd, wr)`
installs them at boot (the kernel passes `arch::pciConfigRead32/Write32`; tests pass a mock).

---

## 2. Decoded device + BARs

`probe(bus, dev, func, out)` reads a slot and fills a `PciDevice` (true iff `vendor != 0xFFFF`):

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

**BAR sizing** (`readBars`) is the standard write-all-ones / read-back trick: save the BAR, write
`0xFFFFFFFF`, read it back, mask the type bits, and `size = ~masked + 1` (a power of two). I/O vs
memory, 64-bit (its high dword occupies the next slot, which is then zeroed), and prefetchable are
decoded from the low flag bits.

---

## 3. API (`kernel/Pci.h`)

| Group | Functions |
|---|---|
| Backend | `setBackend(rd, wr)`, `hasBackend()` |
| Raw config | `read8/16/32`, `write8/16/32(bus, dev, func, off)` |
| Discovery | `probe(slot, out)`, `enumerate(out, max)` (brute-force scan → count), `find(vendor, device, out)` (`0xFFFF` = wildcard) |
| Command bits (RMW) | `enableBusMaster(d)`, `enableMemSpace(d)`, `enableIoSpace(d)` |

Standard offsets are in the header (`PCI_VENDOR_ID … PCI_BAR0 … PCI_IRQ_LINE`), with
`PCI_CMD_IO/MEM/MASTER` and `PCI_VENDOR_NONE = 0xFFFF`.

---

## 4. Boot wiring & driver use

`Kernel::start` (@docs/boot.md §4) installs the backend and scans:

```cpp
Pci::setBackend(arch::pciConfigRead32, arch::pciConfigWrite32);
// pciScanReport(): enumerate() the bus, log the count, find(0x8086, 0x100E) → the e1000
```

This runs **before** the kexts load, because a NIC driver finds its device through PCI. A driver
kext does not call `kernel::Pci` directly — it goes through the stable `knx_pci_*` exports
(@docs/kext.md §4), which wrap these functions: `knx_pci_find` → `Pci::find`, `knx_pci_bar`/
`knx_pci_bar_size`/`knx_pci_bar_is_io` → the decoded `PciBar`, `knx_pci_irq` → `irqLine`,
`knx_pci_enable_bus_master` → `Pci::enableBusMaster`, plus raw `knx_pci_cfg_read32/write32`. So
adding a PCI driver is: `find` the device, read a BAR, `knx_map_mmio` it, enable bus mastering, hook
the IRQ (@docs/networking.md §2.1 for the e1000 doing exactly this).

> Contrast with Linux: no PCI driver model / hotplug / MSI-X / capability walking / power
> management — a flat brute-force scan and BAR decode, enough to bind the one NIC.

**Key files:** `kernel/Pci.{h,cpp}`, `arch/include/arch/pci.h`, `arch/x86/io/pci_x86.cpp`,
`kernel/Kernel.cpp` (scan), `tests/test_pci.cpp`.
