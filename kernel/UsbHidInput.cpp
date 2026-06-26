// kernel/UsbHidInput.cpp — in-kernel USB-HID input. See UsbHidInput.h.
#include "UsbHidInput.h"
#include "UsbCore.h"
#include "UsbHid.h"
#include "MouseDevice.h"     // kext::MouseDevice (compiled into the kernel for the USB mouse)
#include "Scheduler.h"
#include "UsbHcLock.h"       // g_usbHcLock: serialize controller access vs MSC commands
#include <arch/usbhc.h>
#include <string.h>

extern "C" void               knx_feed_scancode(unsigned char sc);
extern "C" int                knx_add_input_dev(kernel::CharDevice* dev);
extern "C" unsigned long long knx_uptime_us(void);
extern "C" void               knx_log(const char* s);

namespace kernel {
namespace {

struct HidDev { int slot, iface, epIn, maxpkt, protocol; bool setup; };  // protocol 1=kbd, 2=mouse
HidDev g_hid[4];
int    g_hidN = 0;
HidKeyboard g_kb;
HidMouse    g_mouse;
kext::MouseDevice* g_mouseDev = 0;

// Keyboard sink: turn a (set-1 scancode, down) edge into the byte stream PS/2 produces — a 0xE0
// prefix for extended keys, then the make code (or make|0x80 on release).
void kbSink(void*, uint8_t keycode, uint8_t down) {
    uint8_t ext = keycode & 0x80, sc = keycode & 0x7F;
    if (ext) knx_feed_scancode(0xE0);
    knx_feed_scancode(down ? sc : (uint8_t)(sc | 0x80));
}
void mRel(void*, int dx, int dy) {
    unsigned long long t = knx_uptime_us();
    if (dx) g_mouseDev->event(kext::EV_REL, kext::REL_X, dx, t);
    if (dy) g_mouseDev->event(kext::EV_REL, kext::REL_Y, dy, t);
}
void mBtn(void*, int code, int down) { g_mouseDev->event(kext::EV_KEY, (unsigned short)code, down, knx_uptime_us()); }
void mSyn(void*)                     { g_mouseDev->event(kext::EV_SYN, kext::SYN_REPORT, 0, knx_uptime_us()); }

// The poll thread: arm + configure each HID endpoint once, then non-blockingly poll every tick.
void usbHidPollBody() {
    const arch::UsbHcOps* ops = arch::usbHcOps();
    static uint8_t report[4][16];   // one DMA buffer per HID device (no cross-device collision)
    for (;;) {
        for (int i = 0; i < g_hidN; i++) {
            // Hold the HC lock across this device's controller work (setup + interrupt poll) so it
            // is mutually exclusive with an in-flight MSC command's CBW/data/CSW on another CPU —
            // otherwise this poll drains that command's completion off the shared event ring. The
            // guard is scoped to the loop body, so it is released before Scheduler::ioWait() below
            // (never yield holding a spinlock).
            SpinGuard hc(g_usbHcLock);
            HidDev& h = g_hid[i];
            if (!h.setup) {
                // SET_PROTOCOL(boot=0) for this interface, then configure its interrupt-IN EP.
                arch::UsbTransfer s{}; s.slot = h.slot; s.type = arch::USB_CONTROL; s.dir = arch::USB_OUT;
                s.setup = { 0x21, 0x0B /*SET_PROTOCOL*/, 0 /*boot*/, (uint16_t)h.iface, 0 };
                ops->submit(arch::usbHc(), &s);
                ops->configureEndpoint(arch::usbHc(), h.slot, h.epIn, arch::USB_INT, arch::USB_IN, h.maxpkt);
                h.setup = true;
            }
            int len = h.maxpkt < 16 ? h.maxpkt : 16;
            // NOTE: do NOT clear the buffer here — the controller may have already DMA'd a completed
            // report into it on a previous iteration; intPoll returns the valid byte count.
            uint8_t* buf = report[i];
            arch::UsbTransfer t{}; t.slot = h.slot; t.endpoint = h.epIn; t.type = arch::USB_INT;
            t.dir = arch::USB_IN; t.data = buf; t.len = (uint32_t)len;
            int n = ops->intPoll(arch::usbHc(), &t);
            if (n > 0) {
                if (h.protocol == 1) hidKeyboardReport(&g_kb, buf, n);
                else                 hidMouseReport(&g_mouse, buf, n);
            }
        }
        Scheduler::ioWait();   // yield until the next tick — never busy-spin on idle HID
    }
}

}  // namespace

void usbHidInit() {
    if (!arch::usbHcOps()) return;
    g_hidN = 0;
    bool haveMouse = false;
    for (int i = 0; i < usbDeviceCount() && g_hidN < 4; i++) {
        const UsbDevice* d = usbDeviceAt(i);
        int proto = 0, iface = 0;
        for (int j = 0; j < d->numInterfaces; j++)
            if (d->iface[j].bInterfaceClass == USB_CLASS_HID) {
                proto = d->iface[j].bInterfaceProtocol;          // 1 = keyboard, 2 = mouse
                iface = d->iface[j].bInterfaceNumber;
            }
        if (proto != 1 && proto != 2) continue;
        int epIn = 0, mp = 8;
        for (int k = 0; k < d->numEndpoints; k++) {
            const UsbEndpointDesc& e = d->endpoint[k];
            if ((e.bmAttributes & 0x3) == 3 && (e.bEndpointAddress & 0x80)) {   // interrupt IN
                epIn = e.bEndpointAddress; mp = e.wMaxPacketSize;
            }
        }
        if (!epIn) continue;
        g_hid[g_hidN++] = { d->slot, iface, epIn, mp, proto, false };
        if (proto == 2) haveMouse = true;
    }
    if (!g_hidN) return;
    knx_log(g_hidN == 1 ? "  USB-HID: 1 device\n" : "  USB-HID: 2+ devices\n");

    g_kb.sink = kbSink;
    if (haveMouse) {
        g_mouseDev = new kext::MouseDevice();
        g_mouse.relSink = mRel; g_mouse.btnSink = mBtn; g_mouse.synSink = mSyn;
        knx_add_input_dev(g_mouseDev);     // -> /dev/input<N>
    }
    Scheduler::create(usbHidPollBody, 4);  // the usbhid poll kernel thread
}

}  // namespace kernel
