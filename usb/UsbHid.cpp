// usb/UsbHid.cpp — boot-protocol HID decoders (MI). See UsbHid.h.
#include "UsbHid.h"
namespace kernel {

// HID keyboard usage -> PS/2 set-1 scancode. bit7 set => the key needs a 0xE0 (extended) prefix.
// (A switch, not a sparse array initializer: GCC C++ rejects non-contiguous designated array init.)
uint8_t hidUsageToScancode(uint8_t u) {
    switch (u) {
        // Letters a..z (usages 0x04..0x1D).
        case 0x04: return 0x1E; case 0x05: return 0x30; case 0x06: return 0x2E; case 0x07: return 0x20;
        case 0x08: return 0x12; case 0x09: return 0x21; case 0x0A: return 0x22; case 0x0B: return 0x23;
        case 0x0C: return 0x17; case 0x0D: return 0x24; case 0x0E: return 0x25; case 0x0F: return 0x26;
        case 0x10: return 0x32; case 0x11: return 0x31; case 0x12: return 0x18; case 0x13: return 0x19;
        case 0x14: return 0x10; case 0x15: return 0x13; case 0x16: return 0x1F; case 0x17: return 0x14;
        case 0x18: return 0x16; case 0x19: return 0x2F; case 0x1A: return 0x11; case 0x1B: return 0x2D;
        case 0x1C: return 0x15; case 0x1D: return 0x2C;
        // Digits 1..9,0 (usages 0x1E..0x27).
        case 0x1E: return 0x02; case 0x1F: return 0x03; case 0x20: return 0x04; case 0x21: return 0x05;
        case 0x22: return 0x06; case 0x23: return 0x07; case 0x24: return 0x08; case 0x25: return 0x09;
        case 0x26: return 0x0A; case 0x27: return 0x0B;
        case 0x28: return 0x1C; /*Enter*/ case 0x29: return 0x01; /*Esc*/
        case 0x2A: return 0x0E; /*Backspace*/ case 0x2B: return 0x0F; /*Tab*/ case 0x2C: return 0x39; /*Space*/
        case 0x2D: return 0x0C; case 0x2E: return 0x0D; case 0x2F: return 0x1A; case 0x30: return 0x1B;
        case 0x31: return 0x2B; case 0x33: return 0x27; case 0x34: return 0x28; case 0x35: return 0x29;
        case 0x36: return 0x33; case 0x37: return 0x34; case 0x38: return 0x35; case 0x39: return 0x3A; /*Caps*/
        // F1..F12 (usages 0x3A..0x45).
        case 0x3A: return 0x3B; case 0x3B: return 0x3C; case 0x3C: return 0x3D; case 0x3D: return 0x3E;
        case 0x3E: return 0x3F; case 0x3F: return 0x40; case 0x40: return 0x41; case 0x41: return 0x42;
        case 0x42: return 0x43; case 0x43: return 0x44; case 0x44: return 0x57; case 0x45: return 0x58;
        // Arrows (extended).
        case 0x4F: return 0x80 | 0x4D; /*Right*/ case 0x50: return 0x80 | 0x4B; /*Left*/
        case 0x51: return 0x80 | 0x50; /*Down*/  case 0x52: return 0x80 | 0x48; /*Up*/
        default: return 0;
    }
}

// Keyboard modifier bit -> set-1 scancode (bit7 = extended). Order = HID modifier byte bits 0..7.
static const uint8_t kModScancode[8] = {
    0x1D /*LCtrl*/, 0x2A /*LShift*/, 0x38 /*LAlt*/, 0x80 | 0x5B /*LGui*/,
    0x80 | 0x1D /*RCtrl*/, 0x36 /*RShift*/, 0x80 | 0x38 /*RAlt*/, 0x80 | 0x5C /*RGui*/,
};

static bool contains(const uint8_t* arr, int n, uint8_t v) {
    for (int i = 0; i < n; i++) if (arr[i] == v) return true;
    return false;
}

void hidKeyboardReport(HidKeyboard* kb, const uint8_t* report, int len) {
    if (len < 8 || !kb->sink) return;
    uint8_t mods = report[0];
    const uint8_t* keys = report + 2;   // 6 key usages

    // Modifier edges.
    for (int b = 0; b < 8; b++) {
        uint8_t now = (mods >> b) & 1, was = (kb->prevMods >> b) & 1;
        if (now != was) kb->sink(kb->user, kModScancode[b], now);
    }
    // Key presses: in `keys` but not in `prev`.
    for (int i = 0; i < 6; i++) {
        uint8_t u = keys[i];
        if (u <= 1) continue;                       // 0 = empty, 1 = rollover error
        if (!contains(kb->prev, 6, u)) {
            uint8_t sc = hidUsageToScancode(u);
            if (sc) kb->sink(kb->user, sc, 1);
        }
    }
    // Key releases: in `prev` but not in `keys`.
    for (int i = 0; i < 6; i++) {
        uint8_t u = kb->prev[i];
        if (u <= 1) continue;
        if (!contains(keys, 6, u)) {
            uint8_t sc = hidUsageToScancode(u);
            if (sc) kb->sink(kb->user, sc, 0);
        }
    }
    for (int i = 0; i < 6; i++) kb->prev[i] = keys[i];
    kb->prevMods = mods;
}

void hidMouseReport(HidMouse* m, const uint8_t* report, int len) {
    if (len < 3) return;
    uint8_t buttons = report[0];
    int dx = (int) (int8_t) report[1];
    int dy = (int) (int8_t) report[2];

    if ((dx || dy) && m->relSink) m->relSink(m->user, dx, dy);
    if (m->btnSink) {
        static const int btnCode[3] = { 0x110 /*BTN_LEFT*/, 0x111 /*BTN_RIGHT*/, 0x112 /*BTN_MIDDLE*/ };
        for (int b = 0; b < 3; b++) {
            uint8_t now = (buttons >> b) & 1, was = (m->prevButtons >> b) & 1;
            if (now != was) m->btnSink(m->user, btnCode[b], now);
        }
    }
    if (m->synSink) m->synSink(m->user);
    m->prevButtons = buttons;
}

}  // namespace kernel
