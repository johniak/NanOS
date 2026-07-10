// usb/UsbMsc.h — USB Mass-Storage Class: Bulk-Only Transport (BOT) + SCSI read (MI, host-testable).
//
// BOT wraps each SCSI command in a 31-byte Command Block Wrapper (bulk-OUT), an optional data
// stage (bulk-IN/OUT), and a 13-byte Command Status Wrapper (bulk-IN). All transfers go through
// the <arch/usbhc.h> host controller, so this is controller-agnostic and host-tested on a mock HC.
#ifndef USB_MSC_H
#define USB_MSC_H
#include <stdint.h>
#include <arch/usbhc.h>
namespace kernel {

struct UsbMsc {
    int slot, epIn, epOut;   // device slot + bulk IN/OUT endpoint addresses (e.g. 0x81 / 0x02)
    uint32_t tag;            // running CBW tag
    uint32_t blockSize;      // learned from READ CAPACITY (defaults to 512)
};

// Configure the bulk endpoints on the controller and reset the MSC state. Returns 0 on success.
int usbMscInit(UsbMsc* m, int slot, int epIn, int epOut);
// SCSI READ CAPACITY(10): fills *blocks (block count) and *blockSize. Returns 0 on success.
int usbMscReadCapacity(UsbMsc* m, uint32_t* blocks, uint32_t* blockSize);
// SCSI READ(10): read `count` blocks starting at `lba` into buf. Returns 0 on success, <0 on error.
int usbMscRead10(UsbMsc* m, uint32_t lba, uint32_t count, void* buf);
// SCSI WRITE(10): write `count` blocks starting at `lba` from buf. Returns 0 on success, <0 on error.
int usbMscWrite10(UsbMsc* m, uint32_t lba, uint32_t count, const void* buf);

// Boot-diagnostic snapshot of the last bot() failure (set on the return path; 0 = last op OK). Read
// by the storage-discovery boot log to explain why a device was skipped. See UsbMsc.cpp for phases.
extern int     g_usbMscFailPhase;   // 0=ok 1=CBW 2=data-short 3=CSW-short 4=CSW-sig 5=CSW-status
extern int     g_usbMscDataMoved;   // bytes moved on a phase-2 short
extern uint8_t g_usbMscCswStatus;   // bCSWStatus on a phase-5 failure

}  // namespace kernel
#endif
