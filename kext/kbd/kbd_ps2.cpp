/*
 * kbd_ps2.cpp — the PS/2 keyboard driver as a loadable module (kbd.nkext), extracted from
 * the kernel. It owns only the hardware: read scancodes on IRQ1 and push them into the
 * kernel's console/evdev input layer via knx_feed_scancode. The line discipline + /dev/input0
 * stay in the kernel (policy, not hardware). nkext_init is the module entry (kext.ld ENTRY).
 */

extern "C" void knx_register_irq(int irq, void (*h)(void*));
extern "C" void knx_feed_scancode(unsigned char sc);
extern "C" void knx_log(const char* s);

namespace {

static inline unsigned char inb(unsigned short port) {
	unsigned char v;
	__asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void outb(unsigned short port, unsigned char v) {
	__asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(port));
}

enum { PS2_DATA = 0x60, PS2_STATUS = 0x64, PS2_CMD = 0x64 };
enum { ST_OUTPUT_FULL = 0x01, ST_INPUT_FULL = 0x02, ST_FROM_AUX = 0x20 };

static void waitWrite() { for (int i = 0; i < 200000; i++) if (!(inb(PS2_STATUS) & ST_INPUT_FULL)) return; }
static unsigned char readData() {
	for (int i = 0; i < 200000; i++) if (inb(PS2_STATUS) & ST_OUTPUT_FULL) break;
	return inb(PS2_DATA);
}

void kbdIrq(void*) {
	unsigned char st = inb(PS2_STATUS);
	if (!(st & ST_OUTPUT_FULL))
		return;
	unsigned char b = inb(PS2_DATA);     // always consume to clear the output buffer
	if (!(st & ST_FROM_AUX))              // keyboard data (mouse data goes via IRQ12)
		knx_feed_scancode(b);
}

}  // namespace

extern "C" int nkext_init() {
	// Enable the keyboard port + IRQ1 in the 8042 config byte, preserving the mouse's bits
	// (bit1 = aux IRQ, bit5 = aux clock). Read-modify-write is order-independent vs the mouse
	// kext because the two modules load sequentially at boot.
	waitWrite(); outb(PS2_CMD, 0x20);     // read controller config byte
	unsigned char cfg = readData();
	cfg |= 0x01;                          // bit0: enable keyboard IRQ (IRQ1)
	cfg &= ~0x10;                         // bit4: enable keyboard clock (clear "disable")
	waitWrite(); outb(PS2_CMD, 0x60);     // write controller config byte
	waitWrite(); outb(PS2_DATA, cfg);
	knx_register_irq(1, kbdIrq);
	// Flush any byte the firmware/GRUB or a keypress made BEFORE this point left in the 8042
	// output buffer. IRQ1 is edge-triggered on new data and the controller will not latch a new
	// byte (nor raise a fresh IRQ) while one is still unread — so a single stale byte from a
	// pre-boot keypress would wedge the keyboard for the whole session. Drain it now that the
	// handler is live (bounded loop; the buffer holds at most a couple of bytes).
	for (int i = 0; i < 32 && (inb(PS2_STATUS) & ST_OUTPUT_FULL); i++)
		(void) inb(PS2_DATA);
	knx_log("  kbd: PS/2 ready\n");
	return 0;
}
