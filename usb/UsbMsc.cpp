// usb/UsbMsc.cpp — BOT + SCSI read over the <arch/usbhc.h> host controller (MI). See UsbMsc.h.
#include "UsbMsc.h"
#include "UsbHcLock.h"
namespace kernel {

// The host-controller serialization lock (see UsbHcLock.h). Defined here; the USB-HID poll thread
// references it via the extern to stay mutually exclusive with MSC commands.
Spinlock g_usbHcLock;

static int bulk(int slot, int ep, arch::UsbDir dir, void* data, uint32_t len) {
    auto ops = arch::usbHcOps(); if (!ops) return -1;
    arch::UsbTransfer t{}; t.slot = slot; t.endpoint = ep; t.type = arch::USB_BULK; t.dir = dir;
    t.data = data; t.len = len;
    if (ops->submit(arch::usbHc(), &t) < 0 || t.result < 0) return -1;
    return t.result;
}

// One Bulk-Only Transport command: CBW out -> optional data -> CSW in. Returns 0 on PASSED.
static int bot(UsbMsc* m, const uint8_t* cdb, int cdbLen, arch::UsbDir dataDir, void* data, uint32_t dataLen) {
    // Serialize the WHOLE command (CBW -> data -> CSW) against the USB-HID poll thread, so it cannot
    // drain this transfer's completion off the shared event ring between our phases. RAII releases
    // on every return path below.
    SpinGuard _hc(g_usbHcLock);
    uint8_t cbw[31] = {0};
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;   // dCBWSignature "USBC"
    uint32_t tag = ++m->tag;
    cbw[4] = (uint8_t)tag; cbw[5] = (uint8_t)(tag >> 8); cbw[6] = (uint8_t)(tag >> 16); cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)dataLen; cbw[9] = (uint8_t)(dataLen >> 8);
    cbw[10] = (uint8_t)(dataLen >> 16); cbw[11] = (uint8_t)(dataLen >> 24);
    cbw[12] = (dataLen && dataDir == arch::USB_IN) ? 0x80 : 0x00;  // bmCBWFlags (bit7: 1=IN)
    cbw[13] = 0;                                                   // bCBWLUN
    cbw[14] = (uint8_t)cdbLen;                                     // bCBWCBLength
    for (int i = 0; i < cdbLen && i < 16; i++) cbw[15 + i] = cdb[i];

    if (bulk(m->slot, m->epOut, arch::USB_OUT, cbw, 31) < 31) return -1;
    if (dataLen) {
        int ep = (dataDir == arch::USB_IN) ? m->epIn : m->epOut;
        // The data phase MUST move the whole dataLen. A real xHCI device can complete a bulk-IN
        // SHORT (cc=13) — xhciSubmit reports that as success with a partial byte count. The old
        // check (< 0 only) then accepted a partially-filled buffer, leaving the PREVIOUS transfer's
        // bytes in the tail. With one READ(10) per 512 B sector, a fully-short read left the entire
        // sector holding stale data: when init read the greeter (login.nxe) right after a getty
        // read toybox (login.nxe), the greeter sector came back as toybox -> "toybox: Unknown
        // command login" and no tty7 login. QEMU always transfers the full 512 B, so it never hit
        // this. Treat any short/failed data phase as a command failure (the block layer retries).
        int moved = bulk(m->slot, ep, dataDir, data, dataLen);
        if (moved < 0 || (uint32_t) moved != dataLen) return -1;
    }
    uint8_t csw[13] = {0};
    if (bulk(m->slot, m->epIn, arch::USB_IN, csw, 13) < 13) return -1;
    if (!(csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 && csw[3] == 0x53)) return -1;  // "USBS"
    if (csw[12] != 0) return -1;                                                              // bCSWStatus != PASSED
    return 0;
}

int usbMscInit(UsbMsc* m, int slot, int epIn, int epOut) {
    m->slot = slot; m->epIn = epIn; m->epOut = epOut; m->tag = 0; m->blockSize = 512;
    auto ops = arch::usbHcOps(); if (!ops) return -1;
    ops->configureEndpoint(arch::usbHc(), slot, epIn,  arch::USB_BULK, arch::USB_IN,  512);
    ops->configureEndpoint(arch::usbHc(), slot, epOut, arch::USB_BULK, arch::USB_OUT, 512);
    return 0;
}

int usbMscReadCapacity(UsbMsc* m, uint32_t* blocks, uint32_t* blockSize) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t reply[8] = {0};
    if (bot(m, cdb, 10, arch::USB_IN, reply, 8) < 0) return -1;
    uint32_t lastLba = ((uint32_t)reply[0] << 24) | ((uint32_t)reply[1] << 16) | ((uint32_t)reply[2] << 8) | reply[3];
    uint32_t bs      = ((uint32_t)reply[4] << 24) | ((uint32_t)reply[5] << 16) | ((uint32_t)reply[6] << 8) | reply[7];
    m->blockSize = bs ? bs : 512;
    if (blocks) *blocks = lastLba + 1;
    if (blockSize) *blockSize = m->blockSize;
    return 0;
}

int usbMscRead10(UsbMsc* m, uint32_t lba, uint32_t count, void* buf) {
    uint32_t bs = m->blockSize ? m->blockSize : 512;
    uint8_t cdb[10] = { 0x28, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    return bot(m, cdb, 10, arch::USB_IN, buf, count * bs) < 0 ? -1 : 0;
}

int usbMscWrite10(UsbMsc* m, uint32_t lba, uint32_t count, const void* buf) {
    uint32_t bs = m->blockSize ? m->blockSize : 512;
    uint8_t cdb[10] = { 0x2A, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    return bot(m, cdb, 10, arch::USB_OUT, (void*)buf, count * bs) < 0 ? -1 : 0;
}

}  // namespace kernel
