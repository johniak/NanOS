// arch/x86_64/boot/entry64.cpp — 64-bit C++ entry reached from the long-mode trampoline
// (loader.S, which leaves the Multiboot info pointer in rdi = arg0). Plan 2: hand that
// pointer to the bootinfo layer, then call the machine-independent kmain(), which runs the
// staged Kernel::start. (Plan 1's direct VGA proof-of-life is now replaced by the real MI
// console path.)

extern "C" void kmain();                       // MI entry (init/kmain.cpp)
extern "C" void bootSetMultibootInfo(unsigned long mb);  // arch-internal (bootinfo_x86_64)

extern "C" void kentry64(unsigned long mb_info) {
	bootSetMultibootInfo(mb_info);             // stash for bootinfo_x86_64 (mmap/framebuffer)
	kmain();                                    // -> staged kernel::Kernel::start()
	for (;;)                                     // backstop: kmain() must not return
		__asm__ __volatile__("hlt");
}
