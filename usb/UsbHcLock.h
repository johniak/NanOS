// usb/UsbHcLock.h — the USB host-controller serialization lock (MI).
//
// One spinlock guards a WHOLE controller operation against another. The two concurrent users are:
//   - USB mass-storage commands (bot(): CBW -> data -> CSW, several submits) run from file/exec
//     reads on any CPU, and
//   - the USB-HID poll thread (usbHidPollBody: SET_PROTOCOL + configure + interrupt poll, drains
//     the shared event ring every tick).
// Per-submit locking is NOT enough: it is released between an MSC command's phases, so the HID
// poll can run in the gap and drain (discard) the MSC transfer's completion off the single shared
// event ring — the read then returns wrong bytes. Holding this lock across the ENTIRE bot() command
// and across the HID poll body makes the two mutually exclusive. Enumeration touches the controller
// too but runs single-threaded at boot (before the HID thread and MSC reads exist), so it needs no
// lock. A plain ticket Spinlock is zero-init safe as a file-scope global (NanOS runs no global
// ctors); SpinGuard keeps IRQs enabled across the (long, busy-poll) transfer, like the block cache.
#ifndef USB_HC_LOCK_H
#define USB_HC_LOCK_H

#include "Spinlock.h"

namespace kernel {
extern Spinlock g_usbHcLock;
}

#endif // USB_HC_LOCK_H
