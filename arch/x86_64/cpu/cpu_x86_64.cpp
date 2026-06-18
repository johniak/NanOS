/*
 * cpu_x86_64.cpp — x86-64 implementation of <arch/cpu.h>.
 *
 * Plan 4 scope: descriptor-table + interrupt-vector bring-up and the interrupt-flag
 * primitives. powerOff/cpuIdentify/rtcEpoch (CPUID/ACPI/CMOS — port semantics identical to
 * i686) are ported in a later plan when MI code that calls them is staged in; archLoadThreadTls
 * is a no-op until %fs.base / TLS arrives in Plan 6.
 */
#include <arch/cpu.h>
#include "Gdt64.h"
#include "Idt64.h"

namespace {
// GDT/IDT live for the kernel's lifetime (the CPU registers point into them). File scope
// (.bss, trivial ctor) so no global constructor is required (the loader calls kmain directly).
kernel::Gdt64 g_gdt;
kernel::Idt64 g_idt;

// Boot kernel stack for the first ring3->ring0 trap (before any scheduler). Once tasks exist
// they supply their own kernel stack via TSS.rsp0 on every switch.
unsigned char g_bootKstack[8192] __attribute__((aligned(16)));
}

namespace arch {

void faultInit();   // arch/x86_64/cpu/fault_x86_64.cpp — #GP/#PF debug handlers

void cpuInit() {
    // GDT first: the IDT gates reference code selector 0x08, valid only once we own the GDT.
    g_gdt.initialize();
    // Point TSS.rsp0 at the boot kernel stack and load the task register, so future
    // ring3->ring0 traps have a kernel stack to land on.
    g_gdt.setKernelStack((uint64_t) (g_bootKstack + sizeof(g_bootKstack)));
    g_gdt.loadTss();
    // IDT: remap the PIC and install all 256 gates (CPU exceptions + IRQs). No sti yet.
    g_idt.initialize();
    faultInit();
}

// Repoint TSS.rsp0 (the kernel stack the CPU loads on a ring3->ring0 interrupt/exception gate).
// The scheduler calls this via setKernelStack (sched_x86_64.cpp) on every task switch; g_gdt is
// file-scoped here, so this thin setter is the way other arch TUs reach it.
void cpuSetTssKernelStack(uint64_t rsp0) { g_gdt.setKernelStack(rsp0); }

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts()  { __asm__ __volatile__("sti"); }
void cpuHalt()              { __asm__ __volatile__("hlt"); }

// Save RFLAGS then disable interrupts; restore (re-enabling IF only if it had been set), so a
// critical section nests correctly regardless of the caller's interrupt state.
unsigned long cpuIrqSave() {
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
void cpuIrqRestore(unsigned long flags) {
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

// TLS on x86-64 uses %fs.base, set up for user threads in Plan 6. No-op until then.
void archLoadThreadTls(unsigned /*base*/) { }

// Power off via the ACPI PM1a control port. QEMU's i440fx exposes it at 0x604 (newer), the
// PIIX4 at 0xB004 (older); 0x4004 covers VirtualBox. SLP_EN|SLP_TYP=0x2000. We try all then
// halt — on real hardware without these ports this just stops the CPU. (Identical to i686:
// port I/O is unchanged in long mode.)
void powerOff() {
    __asm__ __volatile__("cli");
    __asm__ __volatile__("outw %0, %1" : : "a"((unsigned short) 0x2000), "Nd"((unsigned short) 0x604));
    __asm__ __volatile__("outw %0, %1" : : "a"((unsigned short) 0x2000), "Nd"((unsigned short) 0xB004));
    __asm__ __volatile__("outw %0, %1" : : "a"((unsigned short) 0x2000), "Nd"((unsigned short) 0x4004));
    for (;;) __asm__ __volatile__("hlt");
}

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
    unsigned char p61 = (unsigned char) ((inb(0x61) & ~0x02) | 0x01);
    outb(0x61, p61);
    outb(0x43, 0xB0);                            // ch2, lobyte/hibyte, mode 0 (one-shot)
    outb(0x42, (unsigned char) (count & 0xFF));
    outb(0x42, (unsigned char) ((count >> 8) & 0xFF));
    unsigned char g = (unsigned char) (inb(0x61) & ~0x01);
    outb(0x61, g);
    outb(0x61, (unsigned char) (g | 0x01));
    unsigned long long t0 = rdtsc();
    unsigned guard = 0;
    while (!(inb(0x61) & 0x20)) {                // wait for ch2 OUT high = terminal count
        if (++guard == 0) break;
    }
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

}  // namespace arch
