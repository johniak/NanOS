/*
 * syscall_x86_64.cpp — x86_64 syscall trap: implements <arch/syscall.h>.
 *
 * SYSCALL/SYSRET (not int 0x80). syscallInit programs the MSRs (EFER.SCE, STAR, LSTAR,
 * FMASK) and the per-CPU block GS points at after swapgs. The asm stub (syscall_entry64.S)
 * builds a kernel::Registers frame and calls syscall_dispatch64, which decodes the AMD64
 * SysV registers (nr in rax; args rdi/rsi/rdx/r10/r8/r9) and forwards to the MI
 * kernel::kernelSyscall. Mirrors arch/x86/cpu/syscall_x86.cpp's syscallTrap.
 */
#include <arch/syscall.h>
#include <arch/usermode.h>
#include <arch/cpu.h>
#include <arch/smp.h>   // smpThisCpu — per-CPU kernel-stack slot
#include "Interrupt64.h"        // kernel::Registers (x86_64 TrapFrame, from Plan 4)
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "SignalDispatch.h"
#include "Exec.h"
#include <stdint.h>

#include "percpu_x86_64.h"

namespace arch {
// One per-CPU block per logical CPU (the array declared in percpu_x86_64.h). The scheduler /
// archEnterUser update the running CPU's kernelStackTop; the SYSCALL stub reads it via %gs
// after swapgs. syscallSetKernelStack + perCpuInitThis are implemented at the bottom of the
// file, where the wrmsr/rdmsr helpers are in scope.
PerCpu g_percpu[MAX_CPUS];
}

namespace {

// MSR numbers.
const uint32_t IA32_EFER          = 0xC0000080;
const uint32_t IA32_STAR          = 0xC0000081;
const uint32_t IA32_LSTAR         = 0xC0000082;
const uint32_t IA32_FMASK         = 0xC0000084;
const uint32_t IA32_KERNEL_GS_BASE= 0xC0000102;

static inline void wrmsr(uint32_t msr, uint64_t v) {
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t) hi << 32) | lo;
}

}  // namespace

extern "C" void syscall_entry();   // syscall_entry64.S

extern "C" void syscall_dispatch64(kernel::Registers* r) {
	// SYSCALL enters with IF=0 (FMASK clears it). Re-enable interrupts for the body, exactly
	// like the int 0x80 path: deferred preemption keeps the kernel non-preemptible, and the
	// sysret restores the caller's RFLAGS (saved in r->rflags / r11).
	arch::cpuEnableInterrupts();
	uint64_t origRax = r->rax;     // syscall number, saved before dispatch (for restart)
	// AMD64 SysV syscall ABI: nr=rax, args=rdi,rsi,rdx,r10,r8,r9.
	long ret = kernel::kernelSyscall((int) r->rax, r->rdi, r->rsi, r->rdx, r->r10, r->r8,
			r->r9, (arch::TrapFrame*) r);
	r->rax = (uint64_t) ret;
	if (kernel::kernelSyscalls()->hasExited())
		kernel::procExit();                          // does not return
	if ((r->cs & 3) == 3)
		kernel::signalDeliver((arch::TrapFrame*) r, (unsigned) origRax, true);
}

namespace arch {

// Program the SYSCALL/SYSRET MSRs on the CALLING CPU. These are per-CPU MSRs, so every CPU that
// will run ring-3 code must do this (the BSP from syscallInit, each AP from the bring-up path).
void syscallInitCpuMsrs() {
	// 1) Enable SYSCALL/SYSRET (EFER.SCE = bit 0).
	wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1);
	// 2) STAR: SYSCALL loads CS=STAR[47:32], SS=+8 (kernel 0x08/0x10); SYSRET computes user
	//    selectors from STAR[63:48] (=0x18 -> CS 0x2B, SS 0x23). See Task 0 GDT layout.
	wrmsr(IA32_STAR, ((uint64_t) 0x08 << 32) | ((uint64_t) 0x18 << 48));
	// 3) LSTAR: the entry RIP.
	wrmsr(IA32_LSTAR, (uint64_t) &syscall_entry);
	// 4) FMASK: bits cleared in RFLAGS on entry. Clear IF (no nested IRQ until we re-enable)
	//    and DF (SysV requires DF=0 in the kernel).
	wrmsr(IA32_FMASK, (1 << 9) | (1 << 10));   // IF | DF
}

void syscallInit() {
	syscallInitCpuMsrs();
	// KERNEL_GS_BASE -> the per-CPU block the stub reads after swapgs. The BSP is CPU 0; APs call
	// perCpuInitThis with their own index in the bring-up path.
	perCpuInitThis(0, 0, 0);
}

// Initialise the calling CPU's per-CPU block and point its GS base at it. Idempotent.
void perCpuInitThis(uint32_t idx, uint32_t lapicId, uint64_t kernelStackTop) {
	PerCpu* pc = &g_percpu[idx];
	pc->cpuIndex = idx;
	pc->lapicId = lapicId;
	pc->kernelStackTop = kernelStackTop;
	pc->userRspScratch = 0;
	pc->currentTask = 0;
	pc->currentThread = 0;
	pc->inIrq = 0;
	wrmsr(IA32_KERNEL_GS_BASE, (uint64_t) pc);   // swapgs on the next kernel entry installs it
}

// Set the kernel stack top the SYSCALL stub loads after swapgs — for the CALLING CPU's per-CPU
// block (the scheduler calls this on every switch; %gs after swapgs points at g_percpu[cpu]).
void syscallSetKernelStack(uint64_t top) { g_percpu[smpThisCpu()].kernelStackTop = top; }

void syscallSelfTest() {
	// No-op on x86_64. The i686 self-test issues `int 0x80` from ring 0, whose handler `iret`s
	// back to ring 0 (same-privilege) — a valid round trip. On x86_64 the fast syscall path is
	// SYSCALL/SYSRET, and SYSRET *unconditionally* returns to ring 3 (CPL 3). A SYSCALL issued
	// from ring 0 would therefore have its `sysret` drop the ring-0 caller into ring 3 at a
	// kernel RIP → #PF. The ISA simply does not support a ring0->ring0 syscall round trip, so
	// there is nothing to self-test here; the real validation is init.nxe (PID 1) issuing
	// syscalls from ring 3, which exercises the entry stub + dispatch + sysret for real.
}

}  // namespace arch
