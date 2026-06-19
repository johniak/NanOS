// arch/include/arch/usbhc.h — MI<->MD USB host-controller contract (included as <arch/usbhc.h>).
#ifndef ARCH_USBHC_H
#define ARCH_USBHC_H
#include <stdint.h>
namespace arch {

enum UsbDir   { USB_OUT = 0, USB_IN = 1 };
enum UsbXfer  { USB_CONTROL = 0, USB_BULK = 2, USB_INT = 3 };
// 8-byte USB SETUP packet (control transfers).
struct UsbSetup { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; } __attribute__((packed));

// One transfer request. `data`/`len` is the data stage (may be 0). For control, `setup` is used.
// `result` is filled by the controller: bytes transferred (>=0) or negative on error/STALL.
struct UsbTransfer {
    int slot;            // device slot id (assigned at address time; 0 before)
    int endpoint;        // endpoint number+dir encoded by the HC (0 = default control EP)
    UsbXfer type;
    UsbDir  dir;
    UsbSetup setup;      // valid for USB_CONTROL
    void*   data;        // buffer (DMA-able; caller owns)
    uint32_t len;
    int     result;      // OUT: >=0 bytes, <0 error
    int     complete;    // OUT: set non-zero by the HC when done (poll model; no callbacks yet)
};

// Opaque controller handle (the MD driver's object). MI never dereferences it.
struct UsbHc;

// Registered by the MD driver at init. MI USB core calls these.
struct UsbHcOps {
    // Reset+enable a freshly-attached port, allocate a device slot, set its address.
    // Returns the slot id (>0) or <0 on failure. Speed reported via the `speed` out param.
    int  (*enablePort)(UsbHc*, int port, int* speed);
    // Configure endpoint(s) for a device after its descriptors are read (max packet, EP types).
    int  (*configureEndpoint)(UsbHc*, int slot, int endpoint, UsbXfer type, UsbDir dir, int maxPacket);
    // Submit a transfer; for the poll model, run it to completion (or until timeout) and fill result.
    int  (*submit)(UsbHc*, UsbTransfer*);
    int  (*portCount)(UsbHc*);
    // Non-blocking interrupt-IN poll (HID): arms one transfer on first call and, on later calls,
    // returns >0 (bytes, into t->data) once a report has arrived and re-arms, 0 if not yet ready,
    // <0 on error. Never blocks — an idle HID endpoint just returns 0 (the caller yields and retries).
    int  (*intPoll)(UsbHc*, UsbTransfer*);
};

// The MD driver registers (hc, ops); MI core fetches the current HC. One HC for now.
void  usbHcRegister(UsbHc* hc, const UsbHcOps* ops);
UsbHc* usbHc();
const UsbHcOps* usbHcOps();

// MD entry: discover + bring up the platform USB host controller(s). The x86_64 impl
// (xhci_x86_64.cpp) PCI-discovers an xHCI and calls usbHcRegister. MI/Kernel calls this
// before the storage stack so a live-USB system can mount its root over USB mass-storage.
// No-op (logs "no xHCI") when no controller is present, so the ATA disk-image path is unaffected.
void  usbHostInit();

}  // namespace arch
#endif
