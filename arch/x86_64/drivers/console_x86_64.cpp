/*
 * console_x86_64.cpp — x86_64 console sink implementing <arch/console.h>.
 *
 * Two backends, mirroring arch/x86/drivers/console_x86.cpp:
 *   - early boot: 80x25 VGA text at physical 0xB8000 (reachable through the loader's 1 GiB
 *     identity map), hardware cursor via VGA ports 0x3D4/0x3D5.
 *   - once the bootloader's linear framebuffer has been mapped into the kernel
 *     (arch::consoleActivateFramebuffer, called from Kernel::initPaging after
 *     mmuMapKernelMmio), the console hands over to the MI FbConsole (Linux fbcon model):
 *     boot text and the shell render as pixel glyphs on the framebuffer. Without this the
 *     VGA text buffer is invisible whenever GRUB set a graphics video mode (the framebuffer
 *     scans out, not 0xB8000), so the fbcon takeover is required for a usable graphics-mode
 *     console — the same wiring the i686 port already has.
 *
 * Port I/O uses inline asm directly (this is MD code, so the MI arch-cleanliness guard does
 * not apply here).
 */
#include <arch/console.h>
#include <arch/bootinfo.h>
#include "FbConsole.h"
#include "Spinlock.h"   // SMP: serialize the shared VGA/framebuffer cell + cursor writes
#include "vt/VtManager.h"   // once VTs are up, the kernel console is VT1 (Linux printk -> tty1)
#include <string.h>

namespace {
// SMP: 4 CPUs printing at once would interleave the VGA cells + cursor. A RECURSIVE IRQ-saving lock
// serializes the sink without deadlocking when a fault/panic-print on the SAME CPU re-enters while
// the lock is held (a lost panic message would be worse than the lock). Cross-CPU it serializes.
kernel::RecursiveSpinlock g_consoleLock;

unsigned short cursorX = 0;
unsigned short cursorY = 0;
volatile unsigned short* videoram = (volatile unsigned short*) 0xB8000;

const unsigned short ATTR = 0x0F00;   // white on black, in the high byte of each cell

// Framebuffer console (Linux fbcon model). Selected at runtime once the bootloader
// framebuffer has been mapped (arch::consoleActivateFramebuffer); until then the VGA text
// path below is used so early boot text is never lost to an unmapped framebuffer.
kernel::FbConsole g_fb;
bool g_useFb = false;

inline void outb(unsigned short port, unsigned char val) {
	__asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
inline unsigned char inb(unsigned short port) {
	unsigned char r;
	__asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(port));
	return r;
}

// COM1 (0x3F8) serial mirror: every console char is also written to the UART so headless QEMU
// runs can capture the full, unbounded log to a file (-serial file:...) — the VGA text buffer
// only retains the last 25 lines. Initialized lazily on first use.
const unsigned short COM1 = 0x3F8;
bool g_serialReady = false;
void serialInit() {
	outb(COM1 + 1, 0x00);   // disable interrupts
	outb(COM1 + 3, 0x80);   // DLAB on
	outb(COM1 + 0, 0x01);   // divisor 1 -> 115200 baud
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);   // 8N1, DLAB off
	outb(COM1 + 2, 0xC7);   // enable+clear FIFO, 14-byte threshold
	outb(COM1 + 4, 0x0B);   // IRQs off, RTS/DSR set
	g_serialReady = true;
}
inline void serialPut(char c) {
	if (!g_serialReady) serialInit();
	while (!(inb(COM1 + 5) & 0x20)) { }   // wait for THR empty
	outb(COM1, (unsigned char) c);
}

void moveCursor() {
	unsigned short loc = (unsigned short) (cursorY * 80 + cursorX);
	outb(0x3D4, 14);
	outb(0x3D5, (unsigned char) (loc >> 8));
	outb(0x3D4, 15);
	outb(0x3D5, (unsigned char) loc);
}

void scroll() {
	if (cursorY >= 25) {
		memcpy((void*) videoram, (void*) (videoram + 80), 24 * 80 * 2);
		for (int i = 24 * 80; i < 25 * 80; i++)
			((unsigned short*) videoram)[i] = (unsigned short) (ATTR | ' ');
		cursorY = 24;
	}
}

}  // namespace

namespace arch {

void consolePutChar(char c) {
	kernel::RecursiveIrqGuard g(g_consoleLock);
	if (c == '\n') serialPut('\r');
	serialPut(c);
	if (kernel::g_vtmgr) {              // VTs up: the kernel console is VT1 (drawn only while VT1 is active)
		kernel::g_vtmgr->write(1, &c, 1);
		return;
	}
	if (g_useFb) {
		g_fb.putChar(c);
		return;
	}
	if (c == 0x08 && cursorX) {
		cursorX--;
	} else if (c == 0x09) {
		cursorX = (unsigned short) ((cursorX + 8) & ~(8 - 1));
	} else if (c == '\r') {
		cursorX = 0;
	} else if (c == '\n') {
		cursorX = 0;
		cursorY++;
	} else if (c >= ' ') {
		videoram[cursorY * 80 + cursorX] = (unsigned short) (ATTR | (unsigned char) c);
		cursorX++;
	}
	if (cursorX >= 80) {
		cursorX = 0;
		cursorY++;
	}
	scroll();
	moveCursor();
}

void consoleClear() {
	kernel::RecursiveIrqGuard g(g_consoleLock);
	if (kernel::g_vtmgr) {
		kernel::g_vtmgr->kernelClear();
		return;
	}
	if (g_useFb) {
		g_fb.clear();
		return;
	}
	const unsigned short blank = (unsigned short) (ATTR | ' ');
	for (int i = 0; i < 80 * 25; i++)
		videoram[i] = blank;
	cursorX = 0;
	cursorY = 0;
	moveCursor();
}

void consoleSetCursor(unsigned x, unsigned y) {
	kernel::RecursiveIrqGuard g(g_consoleLock);
	if (kernel::g_vtmgr) {
		kernel::g_vtmgr->kernelSetCursor(x, y);
		return;
	}
	if (g_useFb) {
		g_fb.setCursor(x, y);
		return;
	}
	cursorX = (unsigned short) x;
	cursorY = (unsigned short) y;
	moveCursor();
}

void consoleInit() {
	consoleClear();
}

// Switch the console onto the bootloader's framebuffer (the fbcon takeover). Called once,
// after the framebuffer MMIO has been identity-mapped into the kernel directory
// (mmuMapKernelMmio in Kernel::initPaging). No-op if the bootloader gave no framebuffer (we
// stay in VGA text mode). The framebuffer physical address is identity-mapped, so it is
// reachable directly as a kernel pointer (same model as the i686 port).
void consoleActivateFramebuffer() {
	const BootFramebuffer* fb = bootFramebuffer();
	if (!fb)
		return;
	kernel::FbSurface s = { (uint8_t*) (uintptr_t) fb->addr, fb->pitch,
			fb->width, fb->height, fb->bpp };
	g_fb.init(s);
	g_useFb = true;
}

void consoleSize(unsigned* cols, unsigned* rows) {
	if (kernel::g_vtmgr) {
		if (cols) *cols = kernel::g_vtmgr->vt(1)->fbcon().cols();
		if (rows) *rows = kernel::g_vtmgr->vt(1)->fbcon().rows();
	} else if (g_useFb) {
		if (cols) *cols = g_fb.cols();
		if (rows) *rows = g_fb.rows();
	} else {
		if (cols) *cols = 80;   // VGA text mode
		if (rows) *rows = 25;
	}
}

// Early-boot POST-code bars. Writes pixels straight at the framebuffer's physical address
// (identity-mapped — by the loader's 64 GiB early map before initPaging, by mmuInitKernel
// after). Assumes 32bpp (every GOP/VBE mode we request is 32bpp). Each call drops one band
// lower so a whole ladder stays on screen; the lowest band reached is the last milestone
// before a reset. Deliberately does NOT touch the FbConsole state, so it works even before
// the console is activated.
void debugBar(unsigned rgb) {
	const BootFramebuffer* fb = bootFramebuffer();
	if (!fb || fb->bpp != 32)
		return;
	static unsigned slot = 0;
	const unsigned band = 28;             // pixels tall per band (incl. a 4 px gap)
	volatile uint32_t* px = (volatile uint32_t*) (uintptr_t) fb->addr;
	unsigned ppl = fb->pitch / 4;         // pixels per scanline (pitch may exceed width*4)
	unsigned y0 = slot * band;
	for (unsigned y = y0; y < y0 + band - 4 && y < fb->height; y++)
		for (unsigned x = 0; x < fb->width; x++)
			px[(unsigned long) y * ppl + x] = rgb;
	slot++;
}

}  // namespace arch
