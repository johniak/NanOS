// usb/UsbCore.h — USB device model + enumeration entry point (MI).
#ifndef USB_CORE_H
#define USB_CORE_H
#include "UsbDescriptors.h"
#include <arch/usbhc.h>
namespace kernel {
struct UsbDevice {
    int slot, speed;
    uint16_t idVendor, idProduct;
    uint8_t  numInterfaces, numEndpoints, configValue;
    UsbInterfaceDesc iface[8];
    UsbEndpointDesc  endpoint[16];
};
// Control GET_DESCRIPTOR helper over the registered HC.
int usbGetDescriptor(int slot, uint8_t type, uint8_t index, void* buf, uint16_t len);
// Full enumeration of `port`: enablePort -> read device desc -> read config -> SET_CONFIGURATION ->
// populate `out`. Returns 0 on success, <0 on error.
int usbEnumeratePort(int port, UsbDevice* out);

// Enumerate EVERY port once into a small registry, so post-boot consumers (USB-HID input, storage
// discovery) can find devices without re-addressing them. Returns the device count.
int usbEnumerateAll();
int usbDeviceCount();
const UsbDevice* usbDeviceAt(int i);
}  // namespace kernel
#endif
