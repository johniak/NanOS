/*
 * arch/cpu.h — MI/MD contract for core CPU control.
 */
#pragma once

namespace arch {

// Bring up the CPU's descriptor tables and interrupt vectors (x86: GDT + IDT +
// PIC). Must be called before enabling interrupts or taking a fault.
void cpuInit();

void cpuDisableInterrupts();
void cpuEnableInterrupts();
void cpuHalt();

// Re-point the thread-local-storage descriptor at `base` (the current thread's TLS block VA)
// and reload the TLS segment register. The scheduler calls this on every context switch with
// the now-current thread's TLS base (0 = the thread has none); set_thread_area also calls it
// when a thread installs its TLS. On x86 this rewrites the single TLS GDT entry and reloads %gs.
void archLoadThreadTls(unsigned base);

// Interrupt-flag save/restore for short critical sections: cpuIrqSave returns the prior EFLAGS
// and disables interrupts; cpuIrqRestore restores them (re-enabling IF only if it was set). Lets
// a critical region nest and stay correct whether the caller already had interrupts off.
unsigned long cpuIrqSave();
void cpuIrqRestore(unsigned long flags);

// Spin-loop relax hint (x86: the PAUSE instruction). Called inside a spinlock's busy-wait to
// cut power + free the pipeline for the lock holder. MD because it is a single CPU instruction.
void cpuRelax();

// Power the machine off (x86: the ACPI/QEMU shutdown ports). Does not return; if the
// platform can't power off it halts forever.
void powerOff();

// CPU identification for /proc/cpuinfo (x86: CPUID). Machine-independent shape so the MI
// /proc layer can render it; the x86 implementation fills it from CPUID leaves.
struct CpuInfo {
	char vendor[16];   // leaf 0 vendor string (e.g. "GenuineIntel", "AuthenticAMD")
	char brand[52];    // leaves 0x80000002-4 brand string, or "" if unsupported
	unsigned family;   // display family/model/stepping (leaf 1)
	unsigned model;
	unsigned stepping;
	unsigned khz;      // measured TSC frequency in kHz (0 if no TSC); for /proc/cpuinfo MHz
	char flags[128];   // space-separated feature flags (fpu, tsc, sse, ...)
};
void cpuIdentify(CpuInfo* out);

// Current wall-clock time as seconds since the Unix epoch, read from the platform's
// real-time clock (x86: the CMOS RTC). Used by clock_gettime(CLOCK_REALTIME) and
// gettimeofday so timestamps are real, not a fabricated fixed epoch. Returns 0 if no RTC.
unsigned rtcEpoch();

// Free-running monotonic microseconds since boot, read from a counter that advances
// independently of interrupts (x86: the invariant TSC scaled by the calibrated frequency).
// Unlike a timer-tick counter, this keeps advancing inside IRQ-disabled / atomic sections —
// required so busy-poll timeouts (e.g. the i915 forcewake-ack wait_for, which runs with
// interrupts off) actually expire instead of spinning forever. Returns 0 if the CPU has no
// usable TSC, so the caller can fall back to the tick clock.
unsigned long long monotonicUs();

}
