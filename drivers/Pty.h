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
#include "WaitQueue.h" // readers/writers of either end park here (event-driven, no tick-poll)

namespace kernel {

// Deliver a terminal-generated signal (Ctrl+C etc.) to the foreground process group.
typedef void (*PtySignalFn)(void* ctx, int sig, int pgrp);

class Pty {
public:
	Pty();

	// Master side (emulator).
	int masterRead(void* buf, unsigned n);          // shell output; -EAGAIN if none, 0 if slave gone
	int masterWrite(const void* buf, unsigned n);   // keystrokes -> line discipline
	// Readable when output is buffered OR the slave has hung up (so a blocked master reader wakes
	// to collect the EOF). Once the slave (shell) has closed every fd, masterRead returns 0.
	bool masterReadable() const { return m_s2mCount > 0 || slaveGone(); }

	// Slave side (shell).
	int slaveRead(void* buf, unsigned n);           // processed input; -EAGAIN if none
	int slaveWrite(const void* buf, unsigned n);    // output -> master (OPOST/ONLCR)
	bool slaveReadable() const { return m_m2sCount > 0; }

	// Slave fd accounting (the shell side): the fd layer bumps these as slave descriptors are
	// opened/duped and dropped. After the slave has been opened and then fully closed, the pty has
	// "hung up" and master reads return EOF (the SSH/telnet server then closes the channel).
	void slaveOpened() { m_slaveRefs++; m_slaveEverOpened = true; }
	void slaveClosed() { if (m_slaveRefs > 0) m_slaveRefs--; }
	bool slaveGone() const { return m_slaveEverOpened && m_slaveRefs == 0; }

	int ioctl(unsigned cmd, void* arg);             // TCGETS/TCSETS/TIOCGWINSZ/...
	void setSignalFn(PtySignalFn fn, void* ctx) { m_sigFn = fn; m_sigCtx = ctx; }
	// One wait list for the whole pty: a reader/writer of EITHER end parks here, and the
	// dispatch wakes it after any read/write on the pair changes a ring's readiness. Shared
	// (so a master write wakes a slave reader); harmless spurious wakeups just re-test.
	WaitQueue* waitQueue() { return &m_wq; }

private:
	static const int CAP = 4096;
	unsigned char m_s2m[CAP]; int m_s2mHead, m_s2mTail, m_s2mCount;   // slave -> master (output)
	unsigned char m_m2s[CAP]; int m_m2sHead, m_m2sTail, m_m2sCount;   // master -> slave (input)
	unsigned char m_line[CAP]; int m_lineLen;                        // canonical pending line
	Termios m_tio;
	Winsize m_win;
	int m_fgPgrp;
	bool m_packet;   // TIOCPKT packet mode: master reads carry a leading status byte (telnetd)
	int m_slaveRefs;        // count of open slave fds (shell side)
	bool m_slaveEverOpened; // a slave fd was opened at least once -> refs hitting 0 means hangup
	PtySignalFn m_sigFn;
	void* m_sigCtx;
	WaitQueue m_wq;

	void s2mPush(unsigned char c);
	bool m2sPush(unsigned char c);          // false if the slave input ring is full
	void echo(unsigned char c);
	bool inputByte(unsigned char c);        // false if the byte could not be enqueued (raw, full)
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
	int mmapInfo(uint64_t*, unsigned*) { return -1; }
	short pollReady(short events) {
		short r = events & POLLOUT;                 // master write never blocks
		if ((events & POLLIN) && m_pty->masterReadable()) r |= POLLIN;
		return r;
	}
	WaitQueue* waitQueue() { return m_pty->waitQueue(); }
};

class PtySlave : public CharDevice {
	Pty* m_pty;
public:
	explicit PtySlave(Pty* p) : m_pty(p) {}
	void open()  { m_pty->slaveOpened(); }    // a shell fd onto the slave: count it
	void close() { m_pty->slaveClosed(); }    // last close -> the pty hangs up, master reads EOF
	int read(unsigned, void* b, unsigned n) { return m_pty->slaveRead(b, n); }
	int write(unsigned, const void* b, unsigned n) { return m_pty->slaveWrite(b, n); }
	int ioctl(unsigned cmd, void* arg) { return m_pty->ioctl(cmd, arg); }
	int mmapInfo(uint64_t*, unsigned*) { return -1; }
	short pollReady(short events) {
		short r = events & POLLOUT;
		if ((events & POLLIN) && m_pty->slaveReadable()) r |= POLLIN;
		return r;
	}
	WaitQueue* waitQueue() { return m_pty->waitQueue(); }
};

}  // namespace kernel
