// usb/UsbHid.h — USB-HID boot-protocol keyboard & mouse decoders (MI, host-testable).
//
// Boot protocol has fixed report layouts (no HID report-descriptor parse needed): the keyboard
// report is 8 bytes [modifiers][reserved][6 key usages]; the mouse report is 3 bytes
// [buttons][dx][dy]. The decoders diff successive reports and emit events through injected sink
// callbacks, so they are testable without the kernel. The usbhid kext wires the sinks to the
// existing evdev path (kbdFeed -> /dev/input0; MouseDevice EV_REL/EV_KEY/EV_SYN).
#ifndef USB_HID_H
#define USB_HID_H
#include <stdint.h>
namespace kernel {

struct HidKeyboard {
    void (*sink)(void* user, uint8_t keycode, uint8_t down) = nullptr;
    void* user = nullptr;
    uint8_t prev[6] = {0, 0, 0, 0, 0, 0};   // previous 6 key usages (for edge detection)
    uint8_t prevMods = 0;                   // previous modifier byte
};
// Decode one 8-byte boot keyboard report, emitting key down/up edges vs the previous report.
// `keycode` is a PS/2 set-1 scancode (bit7 = extended/0xE0 key); 0-mapped usages are skipped.
void hidKeyboardReport(HidKeyboard* kb, const uint8_t* report, int len);

struct HidMouse {
    void (*relSink)(void* user, int dx, int dy) = nullptr;
    void (*btnSink)(void* user, int button, int down) = nullptr;   // button = evdev BTN_* code
    void (*synSink)(void* user) = nullptr;
    void* user = nullptr;
    uint8_t prevButtons = 0;
};
// Decode one 3-byte boot mouse report: relative move + button edges, then one SYN.
void hidMouseReport(HidMouse* m, const uint8_t* report, int len);

// HID Usage Page 0x07 (Keyboard/Keypad) usage -> PS/2 set-1 scancode. bit7 = extended (0xE0) key.
// Returns 0 for unmapped usages.
uint8_t hidUsageToScancode(uint8_t usage);

}  // namespace kernel
#endif
