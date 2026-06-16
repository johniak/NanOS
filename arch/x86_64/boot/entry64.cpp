// arch/x86_64/boot/entry64.cpp — minimal 64-bit C++ entry point reached from the
// long-mode trampoline (loader.S). Its sole job in Plan 1: prove the x86_64-elf
// toolchain compiles/links freestanding C++ AND that we are genuinely executing 64-bit
// code in long mode, by writing a message straight to the VGA text buffer at 0xB8000.
// The full MI kmain wiring comes in later plans.

extern "C" void kentry64(unsigned long mb_info) {
    (void) mb_info;  // multiboot info ptr — unused in Plan 1, wired in Plan 2
    volatile unsigned short* vga = (volatile unsigned short*) 0xB8000;
    const char* msg = "NanOS x86_64 long mode OK";
    unsigned i = 0;
    for (; msg[i] != '\0'; ++i)
        vga[i] = (unsigned short) (0x2F00 | (unsigned char) msg[i]);  // 0x2F = white on green
    // clear the rest of the first row so a stale framebuffer doesn't confuse the eye
    for (; i < 80; ++i)
        vga[i] = 0x2F20;  // space, same attribute
    for (;;)
        __asm__ __volatile__("hlt");
}
