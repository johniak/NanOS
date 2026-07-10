/*
 * arch/sched.h — MI/MD contract for the scheduler's machine-dependent bits.
 *
 * The MI Scheduler owns the task table, states and round-robin policy; the arch
 * provides the actual CPU context switch (stack-pointer swap + CR3), the fabricated
 * first-run stack, and the preemption timer.
 */
#pragma once

#include <stdint.h>

namespace kernel { struct Task; }   // scheduler task (Scheduler.h)

namespace arch {

struct TrapFrame;   // opaque syscall trap frame (the x86 Registers)

// Address-typed parameters use uintptr_t (kernel stack pointers) / uint64_t (CR3 phys) so the
// SAME contract serves i686 (32-bit) and x86_64 (64-bit): uintptr_t == uint32_t on i686-elf, so
// the i686 impl behaviour is unchanged, while the x86_64 impl gets 64-bit stack pointers + CR3.

// Save the current context (callee-saved regs + esp + CR3) into *saveOldKesp, then
// load newKesp's context. Returns (on this task) when something switches back to it.
//
// saveOldFx/loadNewFx: per-task 512-byte 16-aligned FXSAVE areas (Task::fx). Ring-3 code is
// built SSE-ON while the kernel is -mno-sse, so at every switch the LIVE XMM0-15/MXCSR/x87
// state is exactly the outgoing user task's — and it was previously NOT saved at all: a
// deferred preemption on ret-to-ring3 lands mid-computation (not at a call boundary), so the
// resumed task continued with ANOTHER task's registers. Silent data corruption; on the Dell it
// presented as "impossible" SIGSEGVs inside a fuzz-proven PNG decoder under boot-time context-
// switch pressure. Either pointer may be null: null saveOldFx skips the save (boot/throwaway
// contexts), null loadNewFx skips the restore. On i686 (cdecl) the extra args are ignored by
// the unchanged 32-bit switch.S — its userland is frozen and predates this fix.
extern "C" void archContextSwitch(uintptr_t* saveOldKesp, uintptr_t newKesp,
                                  void* saveOldFx, void* loadNewFx);

// Capture the LIVE FPU/SSE state into a task's FXSAVE area — fork/clone inherit the parent's
// x87/MXCSR control state (POSIX: the child is a copy; rounding modes must survive fork).
// x86_64 only; the i686 impl is a no-op (its userland does not use SSE).
void archFpuCapture(void* fx);

// Load a FXSAVE area into the LIVE FPU/SSE registers — execve resets the surviving task to the
// ABI-default state without waiting for the next context switch. i686: no-op.
void archFpuLoad(const void* fx);

// Fabricate a fresh task's kernel stack so the first archContextSwitch into the
// returned kesp "returns" into the task trampoline (which runs the task body).
uintptr_t archTaskBootstrap(unsigned char* kstackTop, uint64_t cr3);

// Physical address of the kernel page directory (initial CR3 for a new task).
uint64_t archKernelCr3();

// Repoint the kernel stack the CPU lands on for the next ring3->ring0 trap (TSS.esp0).
// The scheduler calls this on every switch so traps land on the running task's stack.
void setKernelStack(uintptr_t esp0);

// Program the preemption timer at `hz` and route its IRQ to the scheduler tick.
void archTimerInit(unsigned hz);

// Idle primitive: enable interrupts and halt until the next one.
void halt_or_hlt();

// fork: fabricate the child task's kernel stack from the parent's syscall trap frame
// so the first context switch into it `ret`s through ret_from_fork and `iret`s the
// copied frame (eax = 0) into ring 3 under `childCr3`. `child` already has kstack/esp0.
void archForkChild(kernel::Task* child, TrapFrame* parentTf, uint64_t childCr3);

// clone (thread create): identical to archForkChild — copy the parent's trap frame, set
// eax=0, build the ret_from_fork context-switch frame — EXCEPT the new thread runs on its
// OWN user stack, so the copied frame's ring-3 ESP is set to `childUserEsp`. `cr3` is the
// SHARED (parent's) page-directory phys (a thread does not get a private address space).
void archCloneChild(kernel::Task* child, TrapFrame* parentTf, uint64_t cr3, uintptr_t childUserEsp);

}
