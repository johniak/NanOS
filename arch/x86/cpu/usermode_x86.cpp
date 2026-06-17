/*
 * usermode_x86.cpp — x86 implementation of <arch/usermode.h>.
 *
 * archLoadUser maps a staged image + heap + stack (and the argv image) into a
 * per-process address space. archEnterUser switches CR3 and irets to ring 3; it does
 * not return — the process runs until it exits (the syscall trap / scheduler take
 * over from there). No longjmp, no nested spawn (Stage 4 trap-frame model).
 */
#include <arch/usermode.h>
#include <arch/mmu.h>
#include "UserStack.h"         // kernel::buildUserStack
#include "PagingControl.h"     // kernel::loadCr3
#include "FrameAllocator.h"    // kernel::g_frames
#include "Interrupt.h"         // kernel::Registers (the x86 TrapFrame)
#include <string.h>

namespace {
// The user window is two 4 MiB PDEs (0x800000..0xFFFFFF, 8 MiB; see mmu_x86.cpp NX_USER_*).
// The program image loads at the bottom (loadBase = 0x800000); the stack lives at the TOP,
// giving the image up to ~7.5 MiB — enough for a large static binary (the OpenSSL CLI). The
// heap is no longer here — sbrk grows a separate high-VA region via SYS_brk (mmuSetUserBrk).
// loadBase stays 0x800000 (must match user/nx.ld); only the window's top (the stack) moved up.
const uint32_t USER_STACK_TOP = 0x1000000;  // top of the 8 MiB user window
const uint32_t USER_STACK_BOT = 0xF80000;   // 512 KiB user stack (top of the user window)
}

namespace arch {

uint32_t archLoadUser(AddressSpace* space, uint32_t loadBase, uint32_t bssEnd,
                      const char* const* argv, int argc,
                      const char* const* envp, int envc) {
	// Image (code/data/bss) -> fresh private USER frames, copying the staged bytes
	// (identity-mapped in the kernel dir). NxeLoader already zeroed the staged bss.
	uint32_t imgEnd = (bssEnd + 0xFFF) & ~0xFFFu;
	for (uint32_t va = loadBase; va < imgEnd; va += 0x1000) {
		uint32_t f = kernel::g_frames.alloc();
		memcpy((void*) f, (void*) va, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}
	// Private zeroed user stack; remember each frame to write the argv image by
	// physical address (the stack is mapped in `space`, not the active directory).
	uint32_t stackFrames[128];   // 512 KiB / 4 KiB
	int sfi = 0;
	for (uint32_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += 0x1000) {
		uint32_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
		stackFrames[sfi++] = f;
	}
	auto stackPhys = [&](uint32_t va) -> uint32_t {
		return stackFrames[(va - USER_STACK_BOT) >> 12] + (va & 0xFFFu);
	};
	return kernel::buildUserStack(USER_STACK_TOP, argv, argc, envp, envc,
		[&](uint32_t va, const void* src, unsigned len) {
			const unsigned char* s = (const unsigned char*) src;
			for (unsigned i = 0; i < len; i++)
				*(unsigned char*) stackPhys(va + i) = s[i];
		});
}

void archLoadModule(AddressSpace* space, uint32_t base, const void* img, uint32_t bytes) {
	// Map a relocated .ndl image into `space` at `base`: fresh private USER frames, the
	// bytes copied in (the loader already applied relocations + zeroed bss in `img`), the
	// last page's tail zeroed. Identity-accessible frames -> call under the kernel dir.
	const unsigned char* src = (const unsigned char*) img;
	uint32_t end = (bytes + 0xFFF) & ~0xFFFu;
	for (uint32_t off = 0; off < end; off += 0x1000) {
		uint32_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		uint32_t n = bytes - off < 0x1000 ? bytes - off : 0x1000;   // off < bytes <= end
		memcpy((void*) f, src + off, n);
		mmuMap(space, base + off, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}
}

void archEnterUser(uint32_t entry, uint32_t userEsp, AddressSpace* space) {
	__asm__ __volatile__("cli");
	mmuSwitch(space);
	__asm__ __volatile__(
			"mov $0x23, %%ax\n\t"   // user data selector (DPL 3)
			"mov %%ax, %%ds\n\t"
			"mov %%ax, %%es\n\t"
			"mov %%ax, %%fs\n\t"
			"mov %%ax, %%gs\n\t"
			"pushl $0x23\n\t"        // ss
			"pushl %0\n\t"           // esp
			"pushl $0x202\n\t"       // eflags (IF=1)
			"pushl $0x1B\n\t"        // cs (user code, DPL 3)
			"pushl %1\n\t"           // eip
			"iret\n\t"
			:
			: "r"(userEsp), "r"(entry)
			: "ax", "memory");
	// not reached
}

namespace {
// What we save on the user stack to resume the interrupted context after the handler.
struct SigContext {
	uint32_t eip, eflags;
	uint32_t eax, ecx, edx, ebx;
	uint32_t esp, ebp, esi, edi;
	uint32_t oldmask;       // low 32 bits of the saved 64-bit signal mask
	uint32_t oldmask_hi;    // high 32 bits (signals 33..64, incl. SIGCANCEL=32)
};
}

void archPushSignalFrame(TrapFrame* tf, uint32_t handler, uint32_t restorer,
                         int sig, uint64_t oldMask, uint32_t origEax, int restartAction) {
	// Delivery runs in the target's own context, so its user CR3 is active and the user
	// stack at useresp is directly writable.
	kernel::Registers* r = (kernel::Registers*) tf;
	uint32_t usp = r->useresp;

	// Decide the eip/eax the program resumes with after the handler returns (sigreturn):
	//  - RESTART: rewind to the int 0x80 (2 bytes) and restore eax = syscall number;
	//  - EINTR:   keep eip, report -EINTR;
	//  - KEEP:    preserve the in-progress eip and the real result in eax.
	uint32_t resumeEip = r->eip;
	uint32_t resumeEax = r->eax;
	if (restartAction == SIG_FRAME_RESTART) { resumeEip = r->eip - 2; resumeEax = origEax; }
	else if (restartAction == SIG_FRAME_EINTR) { resumeEax = (uint32_t) (-4); }   // -EINTR

	usp -= sizeof(SigContext);              // save the interrupted register context
	SigContext* ctx = (SigContext*) usp;
	ctx->eip = resumeEip;  ctx->eflags = r->eflags;
	ctx->eax = resumeEax;  ctx->ecx = r->ecx;
	ctx->edx = r->edx;     ctx->ebx = r->ebx;
	ctx->esp = r->useresp; ctx->ebp = r->ebp;
	ctx->esi = r->esi;     ctx->edi = r->edi;
	ctx->oldmask    = (uint32_t) oldMask;
	ctx->oldmask_hi = (uint32_t) (oldMask >> 32);

	usp -= 4; *(uint32_t*) usp = (uint32_t) sig;        // handler's cdecl arg1
	usp -= 4; *(uint32_t*) usp = restorer;              // handler's return address

	r->eip = handler;       // iret drops into the handler ...
	r->useresp = usp;       // ... on the freshly built frame
	r->eflags &= ~0x400u;   // clear DF for the handler (SysV entry convention)
}

int archSigreturn(TrapFrame* tf, uint64_t* oldMaskOut) {
	kernel::Registers* r = (kernel::Registers*) tf;
	// The trampoline popped the signum, so useresp now points at the saved context.
	const SigContext* ctx = (const SigContext*) r->useresp;
	uint32_t savedEax = ctx->eax;

	r->eip = ctx->eip;
	r->eflags = (ctx->eflags & 0xCD5u) | 0x202u;   // sanitize: keep status+DF, force IF, IOPL=0
	r->ecx = ctx->ecx; r->edx = ctx->edx; r->ebx = ctx->ebx;
	r->ebp = ctx->ebp; r->esi = ctx->esi; r->edi = ctx->edi;
	r->useresp = ctx->esp;          // restore the original user esp
	r->eax = savedEax;

	if (oldMaskOut)
		*oldMaskOut = ((uint64_t) ctx->oldmask_hi << 32) | ctx->oldmask;
	return (int) savedEax;
}

int archSyscallResult(TrapFrame* tf) {
	return (int) ((kernel::Registers*) tf)->eax;
}

void archRestartSyscall(TrapFrame* tf, uint32_t origEax) {
	kernel::Registers* r = (kernel::Registers*) tf;
	r->eip -= 2;        // back up over the 2-byte `int 0x80`
	r->eax = origEax;   // restore the syscall number so the iret re-issues it
}

void archFrameToUser(TrapFrame* tf, uint32_t entry, uint32_t userEsp) {
	// The x86 trap frame IS kernel::Registers on the trapping task's kernel stack.
	// Rewrite it so the ISR's tail `iret` drops into ring 3 at the new image.
	kernel::Registers* r = (kernel::Registers*) tf;
	r->eip = entry;
	r->useresp = userEsp;
	r->cs = 0x1B;          // user code, DPL 3
	r->ss = 0x23;          // user stack, DPL 3
	r->ds = 0x23;          // user data (restored by the ISR tail into ds/es/fs/gs)
	r->eflags = 0x202;     // IF=1
	r->eax = 0;
}

// i686 keeps the user thread pointer in a GDT %gs entry, not an MSR — no-op here.
void archSetUserFsBase(uint64_t) {}

}  // namespace arch
