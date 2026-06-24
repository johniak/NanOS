#include "vt/VtManager.h"

namespace kernel {

VtManager* g_vtmgr = 0;

void VtManager::init(const FbSurface& s, VtSignalFn sig) {
	m_surf = s;
	m_signal = sig;
	for (int i = 1; i <= kVtCount; i++) m_vt[i].init(s, i);
	m_vt[kVtGraphics].setMode(KD_GRAPHICS);  // F7 is graphics by default
	m_active = 1;
	m_pending = m_irqPending = m_relDeadline = 0;
	m_vt[1].fbcon().repaintAll();            // VT1 is the live console at boot
}

// Make VT n the owning/visible console: a text VT blits its whole grid; a process-mode graphics
// VT is told to redraw (acqsig) and the kernel draws nothing on it.
void VtManager::acquire(int n) {
	VtConsole& v = m_vt[n];
	if (v.mode() == KD_GRAPHICS) {
		if (v.vtMode() == VT_PROCESS && m_signal && v.ownerPid())
			m_signal(v.ownerPid(), v.acqsig());
	} else {
		v.fbcon().repaintAll();
	}
}

bool VtManager::switchTo(int n) {
	if (n < 1 || n > kVtCount || n == m_active) return true;
	VtConsole& cur = m_vt[m_active];
	// Releasing a process-mode graphics VT: ask the owner, park until VT_RELDISP (or a timeout).
	if (cur.mode() == KD_GRAPHICS && cur.vtMode() == VT_PROCESS && m_signal && cur.ownerPid()) {
		cur.setRelWait(true);
		m_pending = n;
		m_relDeadline = kVtReleaseTimeoutTicks;
		m_signal(cur.ownerPid(), cur.relsig());
		return false;                            // completes in relDisp() / releaseTimeoutTick()
	}
	// Text VT (or auto-mode graphics): release is immediate.
	cur.fbcon().setLive(false);
	m_active = n;
	acquire(n);
	return true;
}

void VtManager::forceCompletePending() {
	if (!m_pending) return;
	m_vt[m_active].setRelWait(false);
	int target = m_pending;
	m_pending = 0;
	m_relDeadline = 0;
	m_active = target;
	acquire(target);
}

void VtManager::relDisp(int n, int arg) {
	VtConsole& v = m_vt[n];
	if (!v.relWait() || m_pending == 0) return;
	if (arg != 1) { v.setRelWait(false); m_pending = 0; m_relDeadline = 0; return; }  // owner refused
	forceCompletePending();
}

void VtManager::requestSwitch(int n) {
	if (n < 1 || n > kVtCount || n == m_active) return;
	VtConsole& cur = m_vt[m_active];
	bool needsRelease = cur.mode() == KD_GRAPHICS && cur.vtMode() == VT_PROCESS && cur.ownerPid();
	if (needsRelease) { m_irqPending = n; return; }   // can't signal+wait in IRQ; defer to thread ctx
	switchTo(n);                                       // text: safe to run now
}

void VtManager::servicePending() {
	if (m_irqPending) { int n = m_irqPending; m_irqPending = 0; switchTo(n); return; }
	releaseTimeoutTick();
}

void VtManager::releaseTimeoutTick() {
	if (!m_pending || !m_vt[m_active].relWait()) return;
	if (m_relDeadline > 0 && --m_relDeadline == 0) forceCompletePending();
}

int VtManager::openqry() const {
	return -1;   // all VTs pre-allocated; nothing "free" to hand out
}

}  // namespace kernel
