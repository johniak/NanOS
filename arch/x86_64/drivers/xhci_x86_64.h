// arch/x86_64/drivers/xhci_x86_64.h — x86_64 xHCI host-controller driver (MD).
// Implements <arch/usbhc.h>: PCI discovery, MMIO, command/event/transfer rings, port reset,
// slot/endpoint contexts. The storage path (this + MI UsbCore + MI UsbMsc) is in-kernel so a
// live-USB system can read its own root.
#ifndef XHCI_X86_64_H
#define XHCI_X86_64_H
namespace arch {
// Internal bring-up; called by usbHostInit() (the <arch/usbhc.h> MD entry point).
void xhciInit();
}  // namespace arch
#endif
