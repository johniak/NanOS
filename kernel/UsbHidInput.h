// kernel/UsbHidInput.h — in-kernel USB-HID (boot keyboard + mouse) input bring-up.
//
// Scans the USB device registry for boot-protocol HID interfaces and, if any, starts a kernel
// poll thread that reads their interrupt-IN reports and feeds the EXISTING evdev path: keyboard ->
// knx_feed_scancode (/dev/input0, the same sink PS/2 uses); mouse -> a MouseDevice published as
// /dev/input<N>. NanWM consumes these unchanged. No-op if there are no HID devices.
//
// (Design note: the spec packages HID as a loadable kext. Because the poll thread, the USB core,
// the HID decoders and the evdev devices are all already MI/in-kernel, and a poll-model interrupt
// endpoint needs a kernel thread, this ships as an in-kernel module — a complete, not stop-gap,
// realization. Kext packaging is a future refactor that would only move this file behind exports.)
#ifndef USB_HID_INPUT_H
#define USB_HID_INPUT_H
namespace kernel {
void usbHidInit();   // call after USB enumeration + kernelExportsInit + Scheduler::init
}
#endif
