// arch/x86_64/drivers/xhci_x86_64.cpp — x86_64 xHCI driver (MD). See xhci_x86_64.h.
//
// Phase 1 (this commit): PCI discovery of the xHCI controller (class 0x0C/0x03/0x30), map its
// MMIO BAR0, read the capability registers (CAPLENGTH/HCIVERSION/HCSPARAMS1) to learn the
// operational-register base + port/slot counts, and log. Controller init + rings + Address
// Device land in the next task. This is ring-0 MD code, so it does its own volatile MMIO.
#include "xhci_x86_64.h"
#include <arch/usbhc.h>
#include <arch/mmu.h>
#include "Pci.h"
#include "Console.h"
#include <stdint.h>

namespace arch {

using kernel::Console;

namespace {

using kernel::Pci;
using kernel::PciDevice;

// xHCI capability-register offsets (bytes from MMIO base; xHCI spec 1.2 §5.3).
enum { CAP_CAPLENGTH = 0x00, CAP_HCIVERSION = 0x02, CAP_HCSPARAMS1 = 0x04 };

volatile uint8_t* g_mmio   = 0;   // MMIO BAR0 base (== capability registers)
volatile uint8_t* g_op     = 0;   // operational registers = MMIO + CAPLENGTH
uint32_t g_mmioPhys        = 0;
int      g_maxPorts        = 0;
int      g_maxSlots        = 0;

inline uint32_t cap32(int off) { return *(volatile uint32_t*) (g_mmio + off); }

// Locate the first xHCI controller: PCI class 0x0C (serial bus), subclass 0x03 (USB),
// prog-IF 0x30 (xHCI). Returns true + fills `out`.
bool findXhci(PciDevice& out) {
    PciDevice devs[32];
    int n = Pci::enumerate(devs, 32);
    for (int i = 0; i < n; i++)
        if (devs[i].classCode == 0x0C && devs[i].subclass == 0x03 && devs[i].progIf == 0x30) {
            out = devs[i];
            return true;
        }
    return false;
}

}  // namespace

void xhciInit() {
    PciDevice d;
    if (!findXhci(d)) {
        Console::writeLine("xHCI: none (continuing on ATA)");
        return;
    }
    // BAR0 is the (possibly 64-bit) MMIO window; the low 32 bits suffice in QEMU's address map.
    g_mmioPhys = d.bar[0].addr;
    uint32_t bytes = d.bar[0].size ? d.bar[0].size : 0x1000;
    Pci::enableMemSpace(d);
    Pci::enableBusMaster(d);
    mmuMapKernelMmio(g_mmioPhys, bytes);
    g_mmio = (volatile uint8_t*) (uintptr_t) g_mmioPhys;

    // The CAPLENGTH/HCIVERSION dword: QEMU's xHCI MMIO only services 32-bit accesses to the
    // capability block, so read the whole dword and slice it (a sub-dword read returns 0).
    uint32_t cap0      = cap32(0x00);
    uint8_t  capLength = (uint8_t)  (cap0 & 0xFF);
    uint16_t hciVer    = (uint16_t) ((cap0 >> 16) & 0xFFFF);
    uint32_t hcs1      = cap32(CAP_HCSPARAMS1);
    g_op       = g_mmio + capLength;
    g_maxSlots = (int) (hcs1 & 0xFF);
    g_maxPorts = (int) ((hcs1 >> 24) & 0xFF);

    Console::write("xHCI: ");
    Console::write(g_maxPorts);   Console::write(" ports, ");
    Console::write(g_maxSlots);   Console::write(" slots, HCIVERSION=");
    Console::writeHex((uint64_t) hciVer);
    Console::write(" @ BAR0=");
    Console::writeHex((uint64_t) g_mmioPhys);
    Console::writeLine("");
}

void usbHostInit() { xhciInit(); }

}  // namespace arch
