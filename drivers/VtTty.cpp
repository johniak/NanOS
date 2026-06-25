#include "VtTty.h"
#include "vt/VtManager.h"
#include "vt/VtConsole.h"
#include "vt/VtIoctl.h"
#include "Termios.h"
#include "Process.h"
#include "Syscall.h"   // EINVAL / ENXIO + Winsize

namespace kernel {

// /dev/tty0 (index 0) is the active VT; /dev/tty1../dev/tty7 carry a fixed index.
int VtTty::resolve() const {
	if (!g_vtmgr) return 0;
	if (m_index >= 1 && m_index <= kVtCount) return m_index;
	return g_vtmgr->active();    // index 0 (and any other sentinel) -> the active VT
}

int VtTty::read(unsigned, void* buf, unsigned n) {
	int idx = resolve();
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(idx) : 0;
	if (!v) return -ENXIO;
	// NON-BLOCKING: return -EAGAIN when no line/byte is ready. The syscall dispatch drives the
	// blocking (sleeps on waitQueue() and retries, honouring O_NONBLOCK + signals) — the same
	// contract the pipe/pty use, so /dev/ttyN integrates with the generic device read loop.
	return v->read((char*) buf, n, true);
}

int VtTty::write(unsigned, const void* buf, unsigned n) {
	int idx = resolve();
	if (!g_vtmgr || idx < 1) return -ENXIO;
	g_vtmgr->write(idx, (const char*) buf, n);   // locked funnel; rasterizes iff this VT is live
	return (int) n;
}

int VtTty::ioctl(unsigned cmd, void* arg) {
	int idx = resolve();
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(idx) : 0;
	if (!v) return -ENXIO;
	switch (cmd) {
	case IOCTL_TIOCGPGRP: if (!arg) return -EINVAL; *(int*) arg = v->fgPgrp(); return 0;
	case IOCTL_TIOCSPGRP: if (!arg) return -EINVAL; v->setFgPgrp(*(int*) arg); return 0;
	case IOCTL_TCGETS:    if (!arg) return -EINVAL; *(Termios*) arg = v->termios(); return 0;
	case IOCTL_TCSETS: case IOCTL_TCSETSW: case IOCTL_TCSETSF:
		if (!arg) return -EINVAL;
		v->termios() = *(const Termios*) arg;
		v->setRaw((v->termios().c_lflag & TL_ICANON) == 0);   // ICANON cleared -> raw line discipline
		return 0;
	case IOCTL_TIOCGWINSZ: {
		if (!arg) return -EINVAL;
		Winsize* ws = (Winsize*) arg;
		ws->ws_col = (unsigned short) v->fbcon().cols();
		ws->ws_row = (unsigned short) v->fbcon().rows();
		ws->ws_xpixel = ws->ws_ypixel = 0;
		return 0;
	}
	case IOCTL_TIOCSCTTY: {
		Process* p = ProcTable::current();
		if (p) p->cttyDev = this;     // this VT becomes the caller's controlling tty (/dev/tty -> here)
		return 0;
	}
	// VT_ACTIVATE / VT_WAITACTIVE take the VT number BY VALUE (Linux ABI), not a pointer.
	case VT_ACTIVATE:   g_vtmgr->switchTo((int) (long) arg); return 0;
	case VT_WAITACTIVE: return 0;   // non-blocking ack (the target is arg-by-value; we don't block)
	case VT_GETSTATE: {
		if (!arg) return -EINVAL;
		vt_stat* st = (vt_stat*) arg;
		st->v_active = (unsigned short) g_vtmgr->active();
		st->v_signal = 0;
		st->v_state = 0;
		for (int i = 1; i <= kVtCount; i++) st->v_state |= (unsigned short) (1u << i);
		return 0;
	}
	case VT_OPENQRY:  if (!arg) return -EINVAL; *(int*) arg = g_vtmgr->openqry(); return 0;
	case KDGETMODE:   if (!arg) return -EINVAL; *(int*) arg = v->mode(); return 0;
	case KDSETMODE:   v->setMode((int) (long) arg); return 0;   // mode is BY VALUE (Linux ABI), not a pointer
	case VT_GETMODE: {
		if (!arg) return -EINVAL;
		vt_mode* vm = (vt_mode*) arg;
		vm->mode = (unsigned char) v->vtMode();
		vm->waitv = 0;
		vm->relsig = (short) v->relsig();
		vm->acqsig = (short) v->acqsig();
		vm->frsig = 0;
		return 0;
	}
	case VT_SETMODE: {
		if (!arg) return -EINVAL;
		vt_mode* vm = (vt_mode*) arg;
		Process* p = ProcTable::current();
		v->setVtMode(vm->mode, vm->relsig, vm->acqsig, (vm->mode == VT_PROCESS && p) ? p->pid : 0);
		return 0;
	}
	case VT_RELDISP:
		g_vtmgr->relDisp(idx, (int) (long) arg);   // arg is an int passed by value (1 = release granted)
		return 0;
	default:
		return -EINVAL;
	}
}

short VtTty::pollReady(short events) {
	int idx = resolve();
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(idx) : 0;
	short r = 0;
	if (v && (events & POLLIN) && v->inputReady()) r |= POLLIN;
	if (events & POLLOUT) r |= POLLOUT;
	return r;
}

WaitQueue* VtTty::waitQueue() {
	int idx = resolve();
	VtConsole* v = g_vtmgr ? g_vtmgr->vt(idx) : 0;
	return v ? v->inputWaitQueue() : 0;
}

// ---- /dev/tty: forward to the caller's controlling terminal (cttyDev) ----------------------
CharDevice* ControllingTty::target() const {
	Process* p = ProcTable::current();
	CharDevice* d = p ? p->cttyDev : 0;
	return (d == this) ? 0 : d;   // never recurse into ourselves
}

int ControllingTty::read(unsigned off, void* buf, unsigned n) {
	CharDevice* d = target();
	return d ? d->read(off, buf, n) : -ENXIO;
}
int ControllingTty::write(unsigned off, const void* buf, unsigned n) {
	CharDevice* d = target();
	return d ? d->write(off, buf, n) : -ENXIO;
}
int ControllingTty::ioctl(unsigned cmd, void* arg) {
	CharDevice* d = target();
	return d ? d->ioctl(cmd, arg) : -ENXIO;
}
int ControllingTty::mmapInfo(uint64_t* p, unsigned* l) {
	CharDevice* d = target();
	return d ? d->mmapInfo(p, l) : -1;
}
short ControllingTty::pollReady(short events) {
	CharDevice* d = target();
	return d ? d->pollReady(events) : 0;
}
WaitQueue* ControllingTty::waitQueue() {
	CharDevice* d = target();
	return d ? d->waitQueue() : 0;
}

}  // namespace kernel
