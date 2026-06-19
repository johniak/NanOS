// usb/UsbCore.cpp — USB enumeration + config-blob walk + HC registry (MI).
#include "UsbCore.h"
#include <string.h>
namespace kernel {
int usbGetDescriptor(int slot, uint8_t type, uint8_t index, void* buf, uint16_t len) {
    auto ops = arch::usbHcOps(); if (!ops) return -1;
    arch::UsbTransfer t{}; t.slot=slot; t.endpoint=0; t.type=arch::USB_CONTROL; t.dir=arch::USB_IN;
    t.setup = { 0x80, 6 /*GET_DESCRIPTOR*/, (uint16_t)((type<<8)|index), 0, len };
    t.data=buf; t.len=len;
    if (ops->submit(arch::usbHc(), &t) < 0 || t.result < 0) return -1;
    return t.result;
}
static int setConfiguration(int slot, uint8_t cfg) {
    auto ops = arch::usbHcOps();
    arch::UsbTransfer t{}; t.slot=slot; t.type=arch::USB_CONTROL; t.dir=arch::USB_OUT;
    t.setup = { 0x00, 9 /*SET_CONFIGURATION*/, cfg, 0, 0 };
    return (ops->submit(arch::usbHc(), &t) < 0 || t.result < 0) ? -1 : 0;
}
int usbEnumeratePort(int port, UsbDevice* out) {
    auto ops = arch::usbHcOps(); if (!ops) return -1;
    memset(out, 0, sizeof *out);
    out->slot = ops->enablePort(arch::usbHc(), port, &out->speed);
    if (out->slot <= 0) return -1;
    UsbDeviceDesc dd{};
    if (usbGetDescriptor(out->slot, USB_DT_DEVICE, 0, &dd, sizeof dd) < (int)sizeof dd) return -1;
    out->idVendor=dd.idVendor; out->idProduct=dd.idProduct;
    uint8_t blob[256];
    int n = usbGetDescriptor(out->slot, USB_DT_CONFIG, 0, blob, sizeof blob);
    if (n < (int)sizeof(UsbConfigDesc)) return -1;
    UsbConfigDesc* c = (UsbConfigDesc*)blob; out->configValue = c->bConfigurationValue;
    // Walk the concatenated config blob by bLength.
    int off = c->bLength;
    while (off + 2 <= n) {
        uint8_t blen = blob[off], btype = blob[off+1];
        if (blen == 0) break;
        if (btype==USB_DT_INTERFACE && out->numInterfaces<8)
            memcpy(&out->iface[out->numInterfaces++], blob+off, sizeof(UsbInterfaceDesc));
        else if (btype==USB_DT_ENDPOINT && out->numEndpoints<16)
            memcpy(&out->endpoint[out->numEndpoints++], blob+off, sizeof(UsbEndpointDesc));
        off += blen;
    }
    if (setConfiguration(out->slot, out->configValue) < 0) return -1;
    return 0;
}
static UsbDevice g_devs[8];
static int g_devCount = 0;
int usbEnumerateAll() {
    auto ops = arch::usbHcOps();
    g_devCount = 0;
    if (!ops) return 0;
    int ports = ops->portCount(arch::usbHc());
    for (int p = 1; p <= ports && g_devCount < 8; p++) {
        UsbDevice d;
        if (usbEnumeratePort(p, &d) == 0) g_devs[g_devCount++] = d;
    }
    return g_devCount;
}
int usbDeviceCount() { return g_devCount; }
const UsbDevice* usbDeviceAt(int i) { return (i >= 0 && i < g_devCount) ? &g_devs[i] : 0; }
}  // namespace kernel

namespace arch {
static UsbHc* g_hc = 0; static const UsbHcOps* g_ops = 0;
void usbHcRegister(UsbHc* hc, const UsbHcOps* ops){ g_hc=hc; g_ops=ops; }
UsbHc* usbHc(){ return g_hc; } const UsbHcOps* usbHcOps(){ return g_ops; }
}  // namespace arch
