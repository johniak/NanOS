/*
 * mouse_ps2.cpp — x86 PS/2 (8042) hardware glue for the mouse kext: initialise the aux
 * device + IRQ12, decode packets into the MI MouseDevice, publish it as /dev/input<N>.
 * This is the machine-dependent half (port I/O + IRQ); it lives in the kext, never the
 * kernel. nkext_init() is the module entry (kext.ld ENTRY).
 */
#include "MouseDevice.h"

// Kernel symbols imported by name (bound by the loader via the kernel export table).
extern "C" void  knx_register_irq(int irq, void (*h)(void*));
extern "C" int   knx_add_input_dev(kernel::CharDevice* dev);
extern "C" unsigned long long knx_uptime_us(void);
extern "C" void  knx_log(const char* s);

namespace {

// ---- x86 port I/O (the kext is x86; no kernel header needed) ----
static inline unsigned char inb(unsigned short port) {
	unsigned char v;
	__asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void outb(unsigned short port, unsigned char v) {
	__asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(port));
}

// 8042 controller ports + status bits.
enum { PS2_DATA = 0x60, PS2_STATUS = 0x64, PS2_CMD = 0x64 };
enum { ST_OUTPUT_FULL = 0x01, ST_INPUT_FULL = 0x02, ST_FROM_AUX = 0x20 };

static void waitWrite() { for (int i = 0; i < 200000; i++) if (!(inb(PS2_STATUS) & ST_INPUT_FULL)) return; }
static void waitRead()  { for (int i = 0; i < 200000; i++) if (inb(PS2_STATUS) & ST_OUTPUT_FULL) return; }
static unsigned char readData()       { waitRead(); return inb(PS2_DATA); }
static void cmd(unsigned char c)       { waitWrite(); outb(PS2_CMD, c); }
static void writeData(unsigned char c) { waitWrite(); outb(PS2_DATA, c); }
// Send a command byte to the AUX (mouse) device: prefix 0xD4, then the byte; returns the ACK.
static unsigned char mouseCmd(unsigned char c) { cmd(0xD4); writeData(c); return readData(); }

kext::MouseDevice* g_mouse = 0;

void mouseIrq(void*) {
	// Drain the controller; only bytes tagged "from aux" are mouse data.
	while (inb(PS2_STATUS) & ST_OUTPUT_FULL) {
		unsigned char st = inb(PS2_STATUS);
		unsigned char b = inb(PS2_DATA);
		if ((st & ST_FROM_AUX) && g_mouse)
			g_mouse->feed(b, knx_uptime_us());
	}
}

}  // namespace

extern "C" int nkext_init() {
	// Enable the aux device and route its IRQ (IRQ12) through the controller config byte.
	cmd(0xA8);                       // enable aux (mouse) port
	cmd(0x20);                       // read controller config byte
	unsigned char cfg = readData();
	cfg |= 0x02;                     // bit1: enable aux (IRQ12) interrupt
	cfg &= ~0x20;                    // bit5: enable aux clock (clear "disable")
	cmd(0x60);                       // write controller config byte
	writeData(cfg);

	// Reset + configure the mouse itself (each command ACKs with 0xFA).
	mouseCmd(0xFF);                  // reset
	readData();                      // 0xAA self-test passed
	readData();                      // 0x00 device id
	mouseCmd(0xF6);                  // set defaults (100 Hz, 3-button)

	g_mouse = new kext::MouseDevice();

	// IntelliMouse "knock": set sample rate 200 -> 100 -> 80, then read the device id. A mouse
	// that supports the scroll wheel reports id 3 and from then on sends 4-byte packets whose
	// 4th byte is the wheel Z. (QEMU's PS/2 mouse implements this.)
	mouseCmd(0xF3); mouseCmd(200);
	mouseCmd(0xF3); mouseCmd(100);
	mouseCmd(0xF3); mouseCmd(80);
	mouseCmd(0xF2);                  // get device id (ACK consumed by mouseCmd)
	unsigned char id = readData();   // the id byte
	if (id == 0x03) {
		g_mouse->setWheel(true);
		knx_log("  mouse: scroll wheel enabled\n");
	}

	mouseCmd(0xF4);                  // enable data reporting

	int n = knx_add_input_dev(g_mouse);   // -> /dev/input<n> (auto-numbered; keyboard is 0)
	knx_register_irq(12, mouseIrq);
	knx_log(n >= 0 ? "  mouse: PS/2 ready\n" : "  mouse: register failed\n");
	return n >= 0 ? 0 : -1;
}
