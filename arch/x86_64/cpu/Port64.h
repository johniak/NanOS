// arch/x86_64/cpu/Port64.h — header-only x86 port I/O for the x86_64 cpu layer.
// Port I/O semantics are identical to i686 (long mode changes nothing here); kept inline
// and header-only so Idt/PIT/keyboard bring-up don't need a separate translation unit.
#pragma once

namespace kernel {

inline void port_outb(unsigned short port, unsigned char value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}
inline unsigned char port_inb(unsigned short port) {
    unsigned char v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
inline unsigned short port_inw(unsigned short port) {
    unsigned short v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

}  // namespace kernel
