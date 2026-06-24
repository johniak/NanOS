#include "vt/VtConsole.h"
#include "Scheduler.h"
#include "SignalDispatch.h"   // consoleSignalGroup, hasPendingSignalCurrent, ERESTARTSYS
#include "Signal.h"           // SIGINT / SIGQUIT / SIGTSTP
#include "Syscall.h"          // EAGAIN

namespace kernel {

void VtConsole::init(const FbSurface& s, int index) {
	m_index = index;
	m_fb.init(s);
	m_fb.setLive(false);          // VtManager makes exactly one console live
	termiosInitCooked(m_termios);
}

void VtConsole::rawPush(unsigned char b) {
	int next = (m_rawHead + 1) % RAWCAP;
	if (next != m_rawTail) { m_rawbuf[m_rawHead] = b; m_rawHead = next; }   // drop on overflow
}
unsigned char VtConsole::rawPop() {
	unsigned char b = m_rawbuf[m_rawTail];
	m_rawTail = (m_rawTail + 1) % RAWCAP;
	return b;
}

bool VtConsole::rawReadyPred(void* self)  { return !((VtConsole*) self)->rawEmpty(); }
bool VtConsole::lineReadyPred(void* self) { return ((VtConsole*) self)->m_line.lineReady(); }

void VtConsole::setRaw(bool r) {
	m_raw = r;
	m_rawHead = m_rawTail = 0;     // drop buffered input on a mode switch
	m_line = LineDiscipline();
	m_decoder = KeyDecoder();
}

void VtConsole::feedScancode(unsigned char sc) {
	if (m_raw) {
		m_decoder.feed(sc, [this](int ev) {
			if (ev < 256) rawPush((unsigned char) ev);
			else {                  // arrow -> ANSI escape sequence
				rawPush(0x1B); rawPush('[');
				rawPush(ev == KEY_UP ? 'A' : ev == KEY_DOWN ? 'B' : ev == KEY_RIGHT ? 'C' : 'D');
			}
		});
	} else {
		auto echo = [this](char e) { m_fb.putChar(e); };   // named lvalue: LineDiscipline::push takes Echo&
		m_decoder.feed(sc, [this, &echo](int ev) {
			// Cooked control keys signal THIS VT's foreground group (per-VT job control).
			if (ev == 0x03) { consoleSignalGroup(SIGINT,  m_fgPgrp); return; }
			if (ev == 0x1C) { consoleSignalGroup(SIGQUIT, m_fgPgrp); return; }
			if (ev == 0x1A) { consoleSignalGroup(SIGTSTP, m_fgPgrp); return; }
			if (ev < 256) m_line.push((char) ev, echo);
			else {                  // arrow -> ANSI escape bytes into the canonical line
				m_line.push(0x1B, echo); m_line.push('[', echo);
				m_line.push(ev == KEY_UP ? 'A' : ev == KEY_DOWN ? 'B' : ev == KEY_RIGHT ? 'C' : 'D', echo);
			}
		});
	}
	if ((m_raw && !rawEmpty()) || (!m_raw && m_line.lineReady()))
		Scheduler::wakeAll(&m_inputWq);
}

int VtConsole::read(char* buf, unsigned n, bool nonblock) {
	if (m_raw) {
		if (nonblock && rawEmpty()) return -EAGAIN;
		Scheduler::sleepOnUntil(&m_inputWq, rawReadyPred, this);
		if (hasPendingSignalCurrent()) return -ERESTARTSYS;
		unsigned i = 0;
		while (i < n && !rawEmpty()) buf[i++] = (char) rawPop();
		return (int) i;
	}
	if (nonblock && !m_line.lineReady()) return -EAGAIN;
	Scheduler::sleepOnUntil(&m_inputWq, lineReadyPred, this);
	if (hasPendingSignalCurrent()) return -ERESTARTSYS;
	return m_line.takeLine(buf, (int) n);
}

bool VtConsole::inputReady() const {
	return m_raw ? !rawEmpty() : const_cast<LineDiscipline&>(m_line).lineReady();
}

void VtConsole::write(const char* buf, unsigned n) {
	for (unsigned i = 0; i < n; i++) m_fb.putChar(buf[i]);
}

}  // namespace kernel
