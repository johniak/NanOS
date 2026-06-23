/*
 * cpu_x86.cpp — x86 implementation of <arch/cpu.h>.
 */
#include <arch/cpu.h>
#include <arch/sched.h>   // setKernelStack contract (uintptr_t esp0)
#include "Gdt.h"
#include "Idt.h"

namespace {
// The GDT/IDT live for the kernel's lifetime: the CPU registers point at the
// descriptor tables held inside these objects. File-scope (.bss, trivial ctor)
// so no global constructor is required.
kernel::Gdt g_gdt;
kernel::Idt g_idt;

// Boot kernel stack for the very first ring3->ring0 trap (the syscall self-test and
// before the scheduler runs). Once the scheduler is live, each task supplies its own
// kernel stack via arch::setKernelStack(task->esp0) on every switch.
unsigned char g_bootKstack[8192] __attribute__((aligned(16)));
}

namespace arch {

void faultInit();   // arch/x86/cpu/fault_x86.cpp — #GP/#PF debug handlers

void cpuInit() {
	// GDT first: the IDT gates use code selector 0x08, valid only once we own
	// the GDT layout (bootloaders differ). Idt::initialize remaps the PIC and
	// installs all 256 gates (incl. the int 0x80 syscall gate) then sti.
	g_gdt.initialize();
	// Point the TSS at the boot kernel stack and load the task register, so
	// ring3->ring0 traps (int 0x80, IRQs) have a kernel stack to switch to until the
	// scheduler starts repointing TSS.esp0 at each task's own stack.
	g_gdt.setKernelStack((unsigned) (g_bootKstack + sizeof(g_bootKstack)));
	g_gdt.loadTss();
	g_idt.initialize();
	faultInit();
	// PS/2 input drivers (keyboard IRQ1, mouse IRQ12) are NOT in the kernel — they are
	// loadable modules (kbd.nkext / mouse.nkext) loaded from /nanos/kext at boot.
}

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts() { __asm__ __volatile__("sti"); }
void cpuHalt() { __asm__ __volatile__("hlt"); }
void cpuRelax() { __asm__ __volatile__("pause" ::: "memory"); }

// Save EFLAGS then disable interrupts; restore EFLAGS (re-enabling IF only if it was set).
// Lets a critical section nest and run correctly whether the caller had interrupts on or off.
unsigned long cpuIrqSave() {
	unsigned long flags;
	__asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) :: "memory");
	return flags;
}
void cpuIrqRestore(unsigned long flags) {
	__asm__ __volatile__("push %0; popf" :: "r"(flags) : "memory", "cc");
}

// Power off via the ACPI PM1a control port. QEMU's i440fx exposes it at 0x604 (newer),
// the PIIX4 at 0xB004 (older); 0x4004 covers VirtualBox. SLP_EN|SLP_TYP=0x2000. We try all
// then halt — on real hardware without these ports this just stops the CPU.
void powerOff() {
	__asm__ __volatile__("cli");
	__asm__ __volatile__("outw %0, %1" : : "a"((unsigned short) 0x2000), "Nd"((unsigned short) 0x604));
	__asm__ __volatile__("outw %0, %1" : : "a"((unsigned short) 0x2000), "Nd"((unsigned short) 0xB004));
	__asm__ __volatile__("outw %0, %1" : : "a"((unsigned short) 0x2000), "Nd"((unsigned short) 0x4004));
	for (;;) __asm__ __volatile__("hlt");
}

// Repoint TSS.esp0 (where the CPU lands on the next ring3->ring0 trap). The scheduler
// calls this on every switch with the next task's kernel-stack top.
void setKernelStack(uintptr_t esp0) { g_gdt.setKernelStack(esp0); }

// Re-point the single TLS GDT descriptor (entry 6, selector 0x33) at the current thread's TLS
// block and reload %gs. Called by the scheduler on every switch and by set_thread_area.
void archLoadThreadTls(unsigned base) { g_gdt.setTlsBase(base); }

// ---- CPU identification via CPUID (for /proc/cpuinfo) --------------------------------
namespace {
inline void cpuid(unsigned leaf, unsigned* a, unsigned* b, unsigned* c, unsigned* d) {
	__asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
inline void put4(char* dst, unsigned v) {
	dst[0] = (char) v; dst[1] = (char) (v >> 8); dst[2] = (char) (v >> 16); dst[3] = (char) (v >> 24);
}
inline int appendFlag(char* dst, int p, int cap, const char* f) {
	if (p && p < cap - 1) dst[p++] = ' ';
	for (int i = 0; f[i] && p < cap - 1; i++) dst[p++] = f[i];
	dst[p] = 0;
	return p;
}
unsigned tscCalibrateKHz();   // defined below (TSC frequency in kHz, cached)
}

void cpuIdentify(CpuInfo* out) {
	unsigned a, b, c, d;
	cpuid(0, &a, &b, &c, &d);              // leaf 0: max leaf + vendor (EBX, EDX, ECX)
	unsigned maxLeaf = a;
	put4(out->vendor + 0, b);
	put4(out->vendor + 4, d);
	put4(out->vendor + 8, c);
	out->vendor[12] = 0;

	out->family = out->model = out->stepping = 0;
	out->flags[0] = 0;
	unsigned featEdx = 0, featEcx = 0;
	if (maxLeaf >= 1) {                    // leaf 1: family/model/stepping + feature bits
		cpuid(1, &a, &b, &c, &d);
		unsigned baseFamily = (a >> 8) & 0xF, baseModel = (a >> 4) & 0xF;
		out->stepping = a & 0xF;
		out->family = baseFamily + ((baseFamily == 0xF) ? ((a >> 20) & 0xFF) : 0);
		out->model = baseModel + ((baseFamily == 0xF || baseFamily == 0x6) ? (((a >> 16) & 0xF) << 4) : 0);
		featEdx = d;
		featEcx = c;
	}
	int p = 0;
	struct { unsigned bit; const char* name; } edxF[] = {
		{ 0, "fpu" }, { 4, "tsc" }, { 5, "msr" }, { 6, "pae" }, { 9, "apic" },
		{ 15, "cmov" }, { 23, "mmx" }, { 25, "sse" }, { 26, "sse2" },
	};
	for (unsigned i = 0; i < sizeof edxF / sizeof edxF[0]; i++)
		if (featEdx & (1u << edxF[i].bit))
			p = appendFlag(out->flags, p, (int) sizeof out->flags, edxF[i].name);
	if (featEcx & (1u << 0))  p = appendFlag(out->flags, p, (int) sizeof out->flags, "sse3");
	if (featEcx & (1u << 19)) p = appendFlag(out->flags, p, (int) sizeof out->flags, "sse4_1");

	out->khz = (featEdx & (1u << 4)) ? tscCalibrateKHz() : 0;   // measure TSC if present

	out->brand[0] = 0;                     // extended leaves 0x80000002-4: brand string
	cpuid(0x80000000u, &a, &b, &c, &d);
	if (a >= 0x80000004u) {
		unsigned* w = (unsigned*) out->brand;
		for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
			cpuid(leaf, &a, &b, &c, &d);
			*w++ = a; *w++ = b; *w++ = c; *w++ = d;
		}
		out->brand[48] = 0;
	}
}

// ---- TSC frequency calibration (for /proc/cpuinfo "cpu MHz") --------------------------
namespace {
inline unsigned char inb(unsigned short port) {
	unsigned char v;
	__asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
inline void outb(unsigned short port, unsigned char v) {
	__asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
inline unsigned long long rdtsc() {
	unsigned lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long) hi << 32) | lo;
}
// Measure the TSC frequency in kHz by gating PIT channel 2 (the speaker timer, NOT the
// system tick on channel 0) for a known ~10 ms and counting TSC ticks across it. Cached
// after the first call (calibration busy-waits). 0 if the CPU lacks a TSC.
unsigned tscCalibrateKHz() {
	static unsigned cached = 0xFFFFFFFFu;
	if (cached != 0xFFFFFFFFu)
		return cached;
	const unsigned PIT_HZ = 1193182u;
	const unsigned MS = 10u;
	unsigned count = PIT_HZ * MS / 1000u;        // ~11932 ticks = 10 ms
	// Enable the channel-2 gate (port 0x61 bit0), keep the speaker off (bit1=0).
	unsigned char p61 = (unsigned char) ((inb(0x61) & ~0x02) | 0x01);
	outb(0x61, p61);
	outb(0x43, 0xB0);                            // ch2, lobyte/hibyte, mode 0 (one-shot)
	outb(0x42, (unsigned char) (count & 0xFF));
	outb(0x42, (unsigned char) ((count >> 8) & 0xFF));
	// Restart the count: toggle the gate low then high.
	unsigned char g = (unsigned char) (inb(0x61) & ~0x01);
	outb(0x61, g);
	outb(0x61, (unsigned char) (g | 0x01));
	unsigned long long t0 = rdtsc();
	unsigned guard = 0;
	while (!(inb(0x61) & 0x20)) {                // wait for ch2 OUT high = terminal count
		if (++guard == 0) break;                 // ~4e9 spin guard (never reached in practice)
	}
	// The delta over ~10 ms fits in 32 bits even at tens of GHz, so cast before dividing
	// (a 32-bit divide is a native instruction; a 64-bit one would need libgcc __udivdi3).
	unsigned dt = (unsigned) (rdtsc() - t0);
	cached = dt / MS;                            // ticks per ms = kHz
	return cached;
}
}

// ---- CMOS real-time clock (for clock_gettime(CLOCK_REALTIME) / gettimeofday) ----------
namespace {
inline unsigned char cmosRead(int reg) {
	unsigned char v;
	__asm__ __volatile__("outb %%al, $0x70" : : "a"((unsigned char) reg));
	__asm__ __volatile__("inb $0x71, %%al" : "=a"(v));
	return v;
}
inline unsigned char bcd2bin(unsigned char v) { return (unsigned char) ((v & 0x0F) + (v >> 4) * 10); }
inline bool isLeap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
}

unsigned rtcEpoch() {
	// Read the CMOS RTC, retrying until two consecutive reads agree (the RTC may update
	// mid-read; bit 7 of register 0x0A is "update in progress").
	unsigned char s = 0, mi = 0, h = 0, d = 0, mo = 0, y = 0;
	unsigned char ls = 0xFF, lmi = 0xFF, lh = 0xFF, ld = 0xFF, lmo = 0xFF, ly = 0xFF;
	for (int tries = 0; tries < 100; tries++) {
		while (cmosRead(0x0A) & 0x80) { }     // wait out update-in-progress
		s = cmosRead(0x00); mi = cmosRead(0x02); h = cmosRead(0x04);
		d = cmosRead(0x07); mo = cmosRead(0x08); y = cmosRead(0x09);
		if (s == ls && mi == lmi && h == lh && d == ld && mo == lmo && y == ly)
			break;
		ls = s; lmi = mi; lh = h; ld = d; lmo = mo; ly = y;
	}
	unsigned char regB = cmosRead(0x0B);
	bool pm = false;
	if (!(regB & 0x04)) {                     // BCD mode -> convert to binary
		pm = (h & 0x80) != 0;                 // preserve the 12-hour PM flag before stripping
		s = bcd2bin(s); mi = bcd2bin(mi); h = bcd2bin((unsigned char) (h & 0x7F));
		d = bcd2bin(d); mo = bcd2bin(mo); y = bcd2bin(y);
	} else {
		pm = (h & 0x80) != 0;
		h = (unsigned char) (h & 0x7F);
	}
	if (!(regB & 0x02)) {                     // 12-hour mode -> normalise to 24-hour
		if (h == 12) h = 0;
		if (pm) h = (unsigned char) (h + 12);
	}
	unsigned year = 2000u + y;                // assume the 21st century (no RTC century reg used)
	if (mo < 1 || mo > 12) return 0;          // garbage RTC -> no time
	static const unsigned cum[] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
	unsigned long long days = 0;
	for (unsigned yy = 1970; yy < year; yy++)
		days += isLeap(yy) ? 366 : 365;
	days += cum[mo - 1];
	if (mo > 2 && isLeap(year)) days += 1;
	days += (d ? d - 1 : 0);
	return (unsigned) (days * 86400ULL + (unsigned) h * 3600u + (unsigned) mi * 60u + s);
}

}
