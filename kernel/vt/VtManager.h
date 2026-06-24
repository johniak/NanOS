/*
 * VtManager.h — owns the virtual consoles + the active index + the real framebuffer.
 *
 * switchTo() drives the Linux VT discipline: text VTs flip a live bit and repaint; a process-mode
 * graphics VT (VT_SETMODE VT_PROCESS) is asked to release (relsig) and the switch completes only
 * when the owner acks via relDisp() (VT_RELDISP) — or a deadline elapses. Signalling is injected
 * (VtSignalFn) so the core is host-testable; the kernel wires it to kernel::signalSend.
 *
 * requestSwitch() is the IRQ entry point (the Ctrl+Alt+Fn matcher): a text switch runs inline
 * (flag flip + repaint, IRQ-safe), but a graphics release must NOT run in IRQ context (it signals
 * and waits), so it is deferred to servicePending(), called from thread context (the scheduler tick).
 */
#pragma once
#include "vt/VtConsole.h"
#include "Framebuffer.h"

namespace kernel {

const int kVtCount = 7;        // tty1..tty7
const int kVtGraphics = 7;     // F7 is the graphics console
const int kVtReleaseTimeoutTicks = 250;   // ~250ms at 1000Hz: force the switch if the owner never acks

typedef void (*VtSignalFn)(int pid, int sig);

class VtManager {
	VtConsole  m_vt[kVtCount + 1];   // 1-based; index 0 unused
	int        m_active = 1;
	int        m_pending = 0;        // a switch awaiting VT_RELDISP (0 = none)
	int        m_irqPending = 0;     // a graphics-release switch requested from IRQ, run in thread ctx
	int        m_relDeadline = 0;    // ticks left before a non-acking owner is forced off
	FbSurface  m_surf{};
	VtSignalFn m_signal = 0;

	void acquire(int n);             // make VT n the owning/visible console
	void forceCompletePending();     // owner acked (or timed out): finish the pending switch
public:
	void init(const FbSurface& s, VtSignalFn sig);
	int  active() const { return m_active; }
	VtConsole* vt(int n) { return (n >= 1 && n <= kVtCount) ? &m_vt[n] : 0; }
	VtConsole* activeVt() { return &m_vt[m_active]; }

	// Returns true if the switch completed synchronously; false if it is pending a release ack.
	bool switchTo(int n);
	void relDisp(int n, int arg);    // VT_RELDISP: arg 1 = release granted -> complete a pending switch
	void requestSwitch(int n);       // IRQ-context entry (Ctrl+Alt+Fn)
	void servicePending();           // thread-context tick: run a deferred switch / age the deadline
	void releaseTimeoutTick();       // age the release deadline by one tick (forces the switch at 0)
	int  openqry() const;            // first free VT (all pre-allocated here) -> -1
};

extern VtManager* g_vtmgr;           // the kernel's instance (null until Kernel.cpp builds it)

}  // namespace kernel
