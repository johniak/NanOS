// usb/UsbDescriptors.h — packed USB descriptor structs + constants (USB 2.0 spec ch.9).
#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
#include <stdint.h>
namespace kernel {
enum { USB_DT_DEVICE=1, USB_DT_CONFIG=2, USB_DT_STRING=3, USB_DT_INTERFACE=4, USB_DT_ENDPOINT=5, USB_DT_HID=0x21 };
enum { USB_CLASS_HID=3, USB_CLASS_MASS_STORAGE=8 };
struct UsbDeviceDesc { uint8_t bLength,bDescriptorType; uint16_t bcdUSB; uint8_t bDeviceClass,bDeviceSubClass,
    bDeviceProtocol,bMaxPacketSize0; uint16_t idVendor,idProduct,bcdDevice; uint8_t iManufacturer,iProduct,
    iSerial,bNumConfigurations; } __attribute__((packed));
struct UsbConfigDesc { uint8_t bLength,bDescriptorType; uint16_t wTotalLength; uint8_t bNumInterfaces,
    bConfigurationValue,iConfiguration,bmAttributes,bMaxPower; } __attribute__((packed));
struct UsbInterfaceDesc { uint8_t bLength,bDescriptorType,bInterfaceNumber,bAlternateSetting,bNumEndpoints,
    bInterfaceClass,bInterfaceSubClass,bInterfaceProtocol,iInterface; } __attribute__((packed));
struct UsbEndpointDesc { uint8_t bLength,bDescriptorType,bEndpointAddress,bmAttributes; uint16_t wMaxPacketSize;
    uint8_t bInterval; } __attribute__((packed));
}  // namespace kernel
#endif
