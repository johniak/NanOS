/*
 * Pty.h — a pseudo-terminal: a master/slave pair joined by two byte rings, with a
 * termios-driven line discipline on the input side (the model in docs/filesystem.md /
 * the terminal plan). The userspace terminal emulator holds the MASTER (/dev/ptmx): it
 * writes keystrokes (run through the line discipline -> the slave's input) and reads the
 * shell's output. The shell holds the SLAVE (/dev/pts0) as stdin/stdout: it reads
 * processed input and writes output (which surfaces on the master).
 *
 *   emulator --write--> [master] --(line discipline: canonical/echo/signals)--> m2s ring
 *                                                                                  |
 *                                                              slave read() <------+
 *   emulator <--read--- [master] <----------------- s2m ring <--- slave write() (OPOST)
 *
 * Pure logic (rings + termios), no scheduler/arch — host-testable. Blocking lives in the
 * dispatch: an empty read returns -EAGAIN and the dispatch waits. The two CharDevice
 * facets (PtyMaster/PtySlave) just forward to the shared Pty.
 */
#pragma once
#include "CharDevice.h"
#include "Termios.h"
#include "Syscall.h"   // EAGAIN, POLLIN/POLLOUT

namespace kernel {

// Deliver a terminal-generated signal (Ctrl+C etc.) to the foreground process group.
typedef void (*PtySignalFn)(void* ctx, int sig, int pgrp);

class Pty {
public:
	Pty();

	// Master side (emulator).
	int masterRead(void* buf, unsigned n);          // shell output; -EAGAIN if none
	int masterWrite(const void* buf, unsigned n);   // keystrokes -> line discipline
	bool masterReadable() const { return m_s2mCount > 0; }

	// Slave side (shell).
	int slaveRead(void* buf, unsigned n);           // processed input; -EAGAIN if none
	int slaveWrite(const void* buf, unsigned n);    // output -> master (OPOST/ONLCR)
	bool slaveReadable() const { return m_m2sCount > 0; }

	int ioctl(unsigned cmd, void* arg);             // TCGETS/TCSETS/TIOCGWINSZ/...
	void setSignalFn(PtySignalFn fn, void* ctx) { m_sigFn = fn; m_sigCtx = ctx; }

private:
	static const int CAP = 4096;
	unsigned char m_s2m[CAP]; int m_s2mHead, m_s2mTail, m_s2mCount;   // slave -> master (output)
	unsigned char m_m2s[CAP]; int m_m2sHead, m_m2sTail, m_m2sCount;   // master -> slave (input)
	unsigned char m_line[CAP]; int m_lineLen;                        // canonical pending line
	Termios m_tio;
	Winsize m_win;
	int m_fgPgrp;
	PtySignalFn m_sigFn;
	void* m_sigCtx;

	void s2mPush(unsigned char c);
	void m2sPush(unsigned char c);
	void echo(unsigned char c);
	void inputByte(unsigned char c);
	void flushLine();
};

// CharDevice facets registered in /dev. They forward to the shared Pty and report poll
// readiness from the relevant ring.
class PtyMaster : public CharDevice {
	Pty* m_pty;
public:
	explicit PtyMaster(Pty* p) : m_pty(p) {}
	int read(unsigned, void* b, unsigned n) { return m_pty->masterRead(b, n); }
	int write(unsigned, const void* b, unsigned n) { return m_pty->masterWrite(b, n); }
	// The master is the emulator side, NOT a controlling terminal: reject the foreground-
	// process-group ioctls so a TIOCGPGRP probe (the SIGTTIN check) treats it as a non-tty
	// and never stops the emulator for reading the master. Other ioctls (termios) pass.
	int ioctl(unsigned cmd, void* arg) {
		if (cmd == IOCTL_TIOCGPGRP || cmd == IOCTL_TIOCSPGRP)
			return -EINVAL;
		return m_pty->ioctl(cmd, arg);
	}
	int mmapInfo(unsigned*, unsigned*) { return -1; }
	short pollReady(short events) {
		short r = events & POLLOUT;                 // master write never blocks
		if ((events & POLLIN) && m_pty->masterReadable()) r |= POLLIN;
		return r;
	}
};

class PtySlave : public CharDevice {
	Pty* m_pty;
public:
	explicit PtySlave(Pty* p) : m_pty(p) {}
	int read(unsigned, void* b, unsigned n) { return m_pty->slaveRead(b, n); }
	int write(unsigned, const void* b, unsigned n) { return m_pty->slaveWrite(b, n); }
	int ioctl(unsigned cmd, void* arg) { return m_pty->ioctl(cmd, arg); }
	int mmapInfo(unsigned*, unsigned*) { return -1; }
	short pollReady(short events) {
		short r = events & POLLOUT;
		if ((events & POLLIN) && m_pty->slaveReadable()) r |= POLLIN;
		return r;
	}
};

}  // namespace kernel
