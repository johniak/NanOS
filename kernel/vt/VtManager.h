/*
 * VtManager.h — owns the virtual consoles + the active index + the real framebuffer.
 *
 * switchTo() drives the Linux VT discipline: text VTs flip a live bit and repaint; a process-mode
 * graphics VT (VT_SETMODE VT_PROCESS) is asked to release (relsig) and the switch completes only
 * when the owner acks via relDisp() (VT_RELDISP) — or a deadline elapses (releaseTimeoutTick,
 * driven by the scheduler tick). Signalling is injected (VtSignalFn) so the core is host-testable;
 * the kernel wires it to kernel::signalSend.
 *
 * Concurrency: ALL framebuffer-console output (kernel printk via write(1,..), shell stdout via
 * write(n,..), keyboard echo via feedActive(), and the switch repaint) funnels through this object
 * under one RECURSIVE IRQ-saving lock — the same role the old single g_consoleLock played, now that
 * output is split across per-VT FbConsoles. requestSwitch()/feedActive() run from the keyboard IRQ;
 * the lock is recursive so a panic-print on the same CPU never self-deadlocks. The per-VT raw input
 * ring stays the lock-free single-producer (IRQ) / single-consumer (read) pattern it always was.
 */
#pragma once
#include "vt/VtConsole.h"
#include "Framebuffer.h"
#include "Spinlock.h"

namespace kernel {

const int kVtCount = 7;        // tty1..tty7
const int kVtGraphics = 7;     // F7 is the graphics console
const int kVtReleaseTimeoutTicks = 250;   // ~250ms at 1000Hz: force the switch if the owner never acks

typedef void (*VtSignalFn)(int pid, int sig);

class VtManager {
	VtConsole  m_vt[kVtCount + 1];   // 1-based; index 0 unused
	int        m_active = 1;
	int        m_pending = 0;        // a switch awaiting VT_RELDISP (0 = none)
	int        m_relDeadline = 0;    // ticks left before a non-acking owner is forced off
	FbSurface  m_surf{};
	VtSignalFn m_signal = 0;
	RecursiveSpinlock m_lock;        // serializes all fb output + switching (old g_consoleLock role)

	void acquire(int n);             // make VT n the owning/visible console (caller holds m_lock)
	void forceCompletePending();     // owner acked (or timed out): finish the pending switch
	bool switchToLocked(int n);
public:
	void init(const FbSurface& s, VtSignalFn sig);
	int  active() const { return m_active; }
	VtConsole* vt(int n) { return (n >= 1 && n <= kVtCount) ? &m_vt[n] : 0; }
	VtConsole* activeVt() { return &m_vt[m_active]; }

	// Returns true if the switch completed synchronously; false if it is pending a release ack.
	bool switchTo(int n);
	void requestSwitch(int n) { switchTo(n); }   // IRQ-context entry (Ctrl+Alt+Fn): safe — no heap/blocking
	void relDisp(int n, int arg);    // VT_RELDISP: arg 1 = release granted -> complete a pending switch
	void releaseTimeoutTick();       // scheduler tick: age the release deadline (forces the switch at 0)
	int  openqry() const;            // first free VT (all pre-allocated here) -> -1

	// Output funnels (take m_lock). write() targets a specific VT (kernel printk -> 1, shell stdout
	// -> its tty); feedActive() runs the active VT's input (echo writes its fbcon).
	void write(int vtIndex, const char* buf, unsigned n);
	void feedActive(unsigned char sc);
	void kernelClear();                          // clear the kernel console (VT1)
	void kernelSetCursor(unsigned x, unsigned y);// position the kernel console (VT1) cursor
};

extern VtManager* g_vtmgr;           // the kernel's instance (null until Kernel.cpp builds it)

}  // namespace kernel
