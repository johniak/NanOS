#include "Pty.h"
#include "Signal.h"   // SIGINT / SIGQUIT / SIGTSTP numbers

namespace kernel {

Pty::Pty() {
	m_s2mHead = m_s2mTail = m_s2mCount = 0;
	m_m2sHead = m_m2sTail = m_m2sCount = 0;
	m_lineLen = 0;
	m_fgPgrp = 0;
	m_sigFn = 0;
	m_sigCtx = 0;
	for (int i = 0; i < NCCS; i++)
		m_tio.c_cc[i] = 0;
	// Sane cooked defaults (like a fresh Linux tty): canonical line editing, echo, signals,
	// CR->NL on input, NL->CRLF on output.
	m_tio.c_iflag = TI_ICRNL;
	m_tio.c_oflag = TO_OPOST | TO_ONLCR;
	m_tio.c_cflag = 0;
	m_tio.c_lflag = TL_ICANON | TL_ECHO | TL_ECHOE | TL_ISIG;
	m_tio.c_line = 0;
	m_tio.c_cc[VINTR] = 3;     // Ctrl+C
	m_tio.c_cc[VQUIT] = 28;    // Ctrl+backslash
	m_tio.c_cc[VERASE] = 127;  // DEL
	m_tio.c_cc[VEOF] = 4;      // Ctrl+D
	m_tio.c_cc[VSUSP] = 26;    // Ctrl+Z
	m_tio.c_cc[VMIN] = 1;
	m_tio.c_cc[VTIME] = 0;
	m_tio.c_ispeed = m_tio.c_ospeed = 38400;
	m_win.ws_row = 25;
	m_win.ws_col = 80;
	m_win.ws_xpixel = m_win.ws_ypixel = 0;
}

void Pty::s2mPush(unsigned char c) {
	if (m_s2mCount >= CAP)
		return;                      // output ring full: drop (emulator drains continuously)
	m_s2m[m_s2mHead] = c;
	m_s2mHead = (m_s2mHead + 1) % CAP;
	m_s2mCount++;
}

void Pty::m2sPush(unsigned char c) {
	if (m_m2sCount >= CAP)
		return;
	m_m2s[m_m2sHead] = c;
	m_m2sHead = (m_m2sHead + 1) % CAP;
	m_m2sCount++;
}

// Echo a typed character back to the master for display (NL -> CRLF so the cursor wraps).
void Pty::echo(unsigned char c) {
	if (c == '\n') { s2mPush('\r'); s2mPush('\n'); }
	else s2mPush(c);
}

// Commit the pending canonical line to the slave's input ring.
void Pty::flushLine() {
	for (int i = 0; i < m_lineLen; i++)
		m2sPush(m_line[i]);
	m_lineLen = 0;
}

// One input byte through the line discipline (per termios).
void Pty::inputByte(unsigned char c) {
	if ((m_tio.c_iflag & TI_ICRNL) && c == '\r')
		c = '\n';
	if (m_tio.c_lflag & TL_ISIG) {                  // signal-generating control chars
		if (c == m_tio.c_cc[VINTR]) { if (m_sigFn) m_sigFn(m_sigCtx, SIGINT, m_fgPgrp); return; }
		if (c == m_tio.c_cc[VQUIT]) { if (m_sigFn) m_sigFn(m_sigCtx, SIGQUIT, m_fgPgrp); return; }
		if (c == m_tio.c_cc[VSUSP]) { if (m_sigFn) m_sigFn(m_sigCtx, SIGTSTP, m_fgPgrp); return; }
	}
	if (m_tio.c_lflag & TL_ICANON) {                // canonical: buffer a line, edit, commit
		if (c == m_tio.c_cc[VERASE] || c == '\b') {
			if (m_lineLen > 0) {
				m_lineLen--;
				if (m_tio.c_lflag & TL_ECHO) { s2mPush('\b'); s2mPush(' '); s2mPush('\b'); }
			}
			return;
		}
		if (c == m_tio.c_cc[VEOF]) {                 // Ctrl+D: commit what we have (EOF if empty)
			flushLine();
			return;
		}
		if (m_tio.c_lflag & TL_ECHO)
			echo(c);
		if (m_lineLen < CAP)
			m_line[m_lineLen++] = c;
		if (c == '\n')
			flushLine();
		return;
	}
	// Raw mode: byte goes straight through (echo if enabled).
	if (m_tio.c_lflag & TL_ECHO)
		echo(c);
	m2sPush(c);
}

int Pty::masterWrite(const void* buf, unsigned n) {
	const unsigned char* p = (const unsigned char*) buf;
	for (unsigned i = 0; i < n; i++)
		inputByte(p[i]);
	return (int) n;
}

int Pty::masterRead(void* buf, unsigned n) {
	if (m_s2mCount == 0)
		return -EAGAIN;
	unsigned char* d = (unsigned char*) buf;
	unsigned r = 0;
	while (r < n && m_s2mCount > 0) {
		d[r++] = m_s2m[m_s2mTail];
		m_s2mTail = (m_s2mTail + 1) % CAP;
		m_s2mCount--;
	}
	return (int) r;
}

int Pty::slaveWrite(const void* buf, unsigned n) {
	const unsigned char* p = (const unsigned char*) buf;
	for (unsigned i = 0; i < n; i++) {
		unsigned char c = p[i];
		if ((m_tio.c_oflag & TO_OPOST) && (m_tio.c_oflag & TO_ONLCR) && c == '\n')
			s2mPush('\r');                          // NL -> CRLF on output
		s2mPush(c);
	}
	return (int) n;
}

int Pty::slaveRead(void* buf, unsigned n) {
	if (m_m2sCount == 0)
		return -EAGAIN;                             // empty (canonical: no full line yet)
	unsigned char* d = (unsigned char*) buf;
	unsigned r = 0;
	while (r < n && m_m2sCount > 0) {
		d[r++] = m_m2s[m_m2sTail];
		m_m2sTail = (m_m2sTail + 1) % CAP;
		m_m2sCount--;
	}
	return (int) r;
}

int Pty::ioctl(unsigned cmd, void* arg) {
	switch (cmd) {
	case IOCTL_TCGETS:
		*(Termios*) arg = m_tio;
		return 0;
	case IOCTL_TCSETS:
	case IOCTL_TCSETSW:
	case IOCTL_TCSETSF:
		m_tio = *(Termios*) arg;
		return 0;
	case IOCTL_TIOCGWINSZ:
		*(Winsize*) arg = m_win;
		return 0;
	case IOCTL_TIOCSWINSZ:
		m_win = *(Winsize*) arg;
		return 0;
	case IOCTL_TIOCGPGRP:
		*(int*) arg = m_fgPgrp;
		return 0;
	case IOCTL_TIOCSPGRP:
		m_fgPgrp = *(int*) arg;
		return 0;
	}
	return -EINVAL;
}

}  // namespace kernel
