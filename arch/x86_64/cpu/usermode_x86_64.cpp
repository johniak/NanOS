/*
 * usermode_x86_64.cpp — x86_64 implementation of <arch/usermode.h>.
 *
 * archLoadUser maps a staged image + a SysV-AMD64 argv/envp stack into a per-process
 * space. archEnterUser sets %fs.base (decision #2 — picolibc TLS errno), switches CR3 and
 * iretq's to ring 3. The signal frame carries all 64-bit GPRs (~2x the i386 one).
 */
#include <arch/usermode.h>
#include <arch/mmu.h>
#include "PagingControl.h"
#include "FrameAllocator.h"
#include "Interrupt64.h"        // kernel::Registers (x86_64 TrapFrame)
#include <string.h>
#include <stdint.h>

namespace arch { void syscallSetKernelStack(uint64_t top); }   // syscall_x86_64.cpp

namespace {
// User window mirrors i686 but in 64-bit low-canonical VA: image at loadBase=0x800000,
// stack at the top of the 8 MiB window. (Plan 3's mmu_x86_64 reserves this band.)
const uint64_t USER_STACK_TOP = 0x1000000;
const uint64_t USER_STACK_BOT = 0xF80000;   // 512 KiB stack

const uint32_t IA32_FS_BASE = 0xC0000100;
inline void wrmsr(uint32_t msr, uint64_t v) {
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}

// Build a SysV-AMD64 argv/envp image at the top of the user stack. Layout (high->low):
// the strings; then a 16-byte-aligned block: argc(8), argv ptrs(8 each)+NULL, envp+NULL.
// rsp ends pointing at argc and MUST be 16-aligned at _start (ABI). Writing is delegated so
// we can poke the child's frames by physical address.
template <class WriteFn>
uint64_t buildUserStack64(uint64_t top, const char* const* argv, int argc,
		const char* const* envp, int envc, WriteFn write) {
	uint64_t sp = top;
	uint64_t aptr[128], eptr[128];
	for (int i = envc - 1; i >= 0; i--) { unsigned l = 0; while (envp[i][l]) l++; l++; sp -= l; write(sp, envp[i], l); eptr[i] = sp; }
	for (int i = argc - 1; i >= 0; i--) { unsigned l = 0; while (argv[i][l]) l++; l++; sp -= l; write(sp, argv[i], l); aptr[i] = sp; }
	uint64_t slots = (uint64_t) (1 + (argc + 1) + (envc + 1));   // argc + argv[]+NULL + envp[]+NULL
	sp -= slots * 8;
	sp &= ~0xFull;                       // 16-align the block base (= rsp at _start)
	uint64_t off = sp;
	uint64_t v = (uint64_t) argc; write(off, &v, 8); off += 8;
	for (int i = 0; i < argc; i++) { write(off, &aptr[i], 8); off += 8; }
	uint64_t nul = 0; write(off, &nul, 8); off += 8;
	for (int i = 0; i < envc; i++) { write(off, &eptr[i], 8); off += 8; }
	write(off, &nul, 8);
	return sp;
}
}  // namespace

namespace arch {

uintptr_t archLoadUser(AddressSpace* space, uintptr_t loadBase, uintptr_t bssEnd,
		const char* const* argv, int argc, const char* const* envp, int envc) {
	uint64_t imgEnd = (bssEnd + 0xFFF) & ~0xFFFull;
	for (uint64_t va = loadBase; va < imgEnd; va += 0x1000) {
		uint64_t f = kernel::g_frames.alloc();
		memcpy((void*) f, (void*) va, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}
	uint64_t stackFrames[128];
	int sfi = 0;
	for (uint64_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += 0x1000) {
		uint64_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
		stackFrames[sfi++] = f;
	}
	auto stackPhys = [&](uint64_t va) -> uint64_t {
		return stackFrames[(va - USER_STACK_BOT) >> 12] + (va & 0xFFFull);
	};
	return (uintptr_t) buildUserStack64(USER_STACK_TOP, argv, argc, envp, envc,
		[&](uint64_t va, const void* src, unsigned len) {
			const unsigned char* s = (const unsigned char*) src;
			for (unsigned i = 0; i < len; i++) *(unsigned char*) stackPhys(va + i) = s[i];
		});
}

void archLoadModule(AddressSpace* space, uintptr_t base, const void* img, uintptr_t bytes) {
	const unsigned char* src = (const unsigned char*) img;
	uint64_t end = (bytes + 0xFFF) & ~0xFFFull;
	for (uint64_t off = 0; off < end; off += 0x1000) {
		uint64_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		uint64_t n = bytes - off < 0x1000 ? bytes - off : 0x1000;
		memcpy((void*) f, src + off, (size_t) n);
		mmuMap(space, base + off, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}
}

void archSetUserFsBase(uint64_t base) { wrmsr(IA32_FS_BASE, base); }

void archEnterUser(uintptr_t entry, uintptr_t userRsp, AddressSpace* space) {
	__asm__ __volatile__("cli");
	mmuSwitch(space);
	// The syscall stub will run on THIS task's kernel stack — point the per-CPU block at it.
	uint64_t krsp; __asm__ __volatile__("mov %%rsp, %0" : "=r"(krsp));
	syscallSetKernelStack(krsp & ~0xFull);
	// %fs.base for picolibc TLS (decision #2). The minimal init (Task 13) uses no TLS, so 0
	// is fine; a picolibc program sets a real thread pointer via arch_prctl(ARCH_SET_FS) and
	// the scheduler reloads it on context switch (Plan 4/8). See the note in the plan.
	archSetUserFsBase(0);
	// iretq down to ring 3: push SS, RSP, RFLAGS(IF=1), CS, RIP.
	__asm__ __volatile__(
		"mov $0x23, %%ax\n\t"     // user data selector (DPL3)
		"mov %%ax, %%ds\n\t"
		"mov %%ax, %%es\n\t"
		"pushq $0x23\n\t"         // ss
		"pushq %0\n\t"            // rsp
		"pushq $0x202\n\t"        // rflags (IF=1)
		"pushq $0x2B\n\t"         // cs (user code64, DPL3)
		"pushq %1\n\t"            // rip
		"iretq\n\t"
		:: "r"((uint64_t) userRsp), "r"((uint64_t) entry) : "rax", "memory");
	// not reached
}

namespace {
// Saved on the user stack to resume the interrupted context after a handler (x86_64).
struct SigContext {
	uint64_t rip, rflags, rsp, rbp;
	uint64_t rax, rbx, rcx, rdx, rsi, rdi;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t oldmask;
};
}

void archPushSignalFrame(TrapFrame* tf, uintptr_t handler, uintptr_t restorer,
		int sig, uint64_t oldMask, uintptr_t origRax, int restartAction) {
	kernel::Registers* r = (kernel::Registers*) tf;
	uint64_t usp = r->rsp;
	uint64_t resumeRip = r->rip, resumeRax = r->rax;
	if (restartAction == SIG_FRAME_RESTART) { resumeRip = r->rip - 2; resumeRax = origRax; }  // back over `syscall` (2 bytes)
	else if (restartAction == SIG_FRAME_EINTR) { resumeRax = (uint64_t) (-4L); }              // -EINTR

	usp &= ~0xFull;                          // keep the user stack 16-aligned
	usp -= sizeof(SigContext);
	SigContext* ctx = (SigContext*) usp;
	ctx->rip = resumeRip; ctx->rflags = r->rflags; ctx->rsp = r->rsp; ctx->rbp = r->rbp;
	ctx->rax = resumeRax; ctx->rbx = r->rbx; ctx->rcx = r->rcx; ctx->rdx = r->rdx;
	ctx->rsi = r->rsi;    ctx->rdi = r->rdi; ctx->r8 = r->r8;   ctx->r9 = r->r9;
	ctx->r10 = r->r10;    ctx->r11 = r->r11; ctx->r12 = r->r12; ctx->r13 = r->r13;
	ctx->r14 = r->r14;    ctx->r15 = r->r15; ctx->oldmask = oldMask;

	usp -= 8; *(uint64_t*) usp = restorer;   // handler's return address -> sigtramp
	r->rdi = (uint64_t) sig;                 // SysV arg1 = signum
	r->rip = handler;
	r->rsp = usp;
	r->rflags &= ~0x400ull;                  // clear DF for the handler
}

int archSigreturn(TrapFrame* tf, uint64_t* oldMaskOut) {
	kernel::Registers* r = (kernel::Registers*) tf;
	const SigContext* ctx = (const SigContext*) r->rsp;   // sigtramp left rsp at the context
	uint64_t savedRax = ctx->rax;
	r->rip = ctx->rip;
	r->rflags = (ctx->rflags & 0xCD5ull) | 0x202ull;
	r->rbx = ctx->rbx; r->rcx = ctx->rcx; r->rdx = ctx->rdx; r->rsi = ctx->rsi; r->rdi = ctx->rdi;
	r->rbp = ctx->rbp; r->r8 = ctx->r8; r->r9 = ctx->r9; r->r10 = ctx->r10; r->r11 = ctx->r11;
	r->r12 = ctx->r12; r->r13 = ctx->r13; r->r14 = ctx->r14; r->r15 = ctx->r15;
	r->rsp = ctx->rsp; r->rax = savedRax;
	if (oldMaskOut) *oldMaskOut = ctx->oldmask;
	return (int) savedRax;
}

int archSyscallResult(TrapFrame* tf) { return (int) ((kernel::Registers*) tf)->rax; }

void archRestartSyscall(TrapFrame* tf, uintptr_t origRax) {
	kernel::Registers* r = (kernel::Registers*) tf;
	r->rip -= 2;            // back over the 2-byte `syscall`
	r->rax = origRax;
}

void archFrameToUser(TrapFrame* tf, uintptr_t entry, uintptr_t userRsp) {
	kernel::Registers* r = (kernel::Registers*) tf;
	r->rip = entry; r->rsp = userRsp;
	r->cs = 0x2B; r->ss = 0x23;
	r->rflags = 0x202;     // IF=1
	r->rax = 0;
}

}  // namespace arch
