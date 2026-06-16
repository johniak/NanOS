#include "Signal.h"

namespace kernel {

static SigMask bit(int sig) { return (SigMask) 1 << (sig - 1); }   // 64-bit: signals 1..64
static bool valid(int sig) { return sig > 0 && sig < NANOS_NSIG; }

// --- ThreadSignals / ProcSignals accessors -------------------------------------------

void ThreadSignals::block(int sig)         { if (valid(sig)) blocked |= bit(sig); }
void ThreadSignals::unblock(int sig)       { if (valid(sig)) blocked &= ~bit(sig); }
bool ThreadSignals::isBlocked(int sig) const { return valid(sig) && (blocked & bit(sig)); }

void ProcSignals::setHandler(int sig, void* h) {
	if (valid(sig))
		handlers[sig] = (unsigned) (unsigned long) h;
}
void* ProcSignals::handler(int sig) const {
	return valid(sig) ? (void*) (unsigned long) handlers[sig] : (void*) 0;
}

// --- pure policy ----------------------------------------------------------------------

SigDefault sigDefaultAction(int sig) {
	switch (sig) {
	case SIGCHLD:
		return SD_IGN;                 // ignored by default
	case SIGCONT:
		return SD_CONT;                // resume a stopped process
	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
		return SD_STOP;                // stop (job control)
	case SIGQUIT:
	case SIGILL:
	case SIGABRT:
	case SIGFPE:
	case SIGSEGV:
	case SIGTRAP:
		return SD_CORE;                // terminate (we have no core dump, treat as TERM)
	default:
		return SD_TERM;                // SIGHUP/SIGINT/SIGKILL/SIGPIPE/SIGALRM/SIGTERM/...
	}
}

bool sigCanCatch(int sig) {
	return sig != SIGKILL && sig != SIGSTOP;
}

int pickSignalTarget(int sig, const ThreadSignals* threads, int count) {
	if (!threads || count <= 0)
		return 0;                          // degenerate: nothing to choose from -> the leader
	// SIGKILL/SIGSTOP are immune to the block mask: any thread (the leader, index 0) takes them.
	bool unblockable = (sig == SIGKILL || sig == SIGSTOP);
	for (int i = 0; i < count; i++)
		if (unblockable || !threads[i].isBlocked(sig))
			return i;                      // first thread that can take it (leader preferred)
	return 0;                              // every thread blocks it -> leave it pending on leader
}

void sigInit(ThreadSignals& s) {
	s.pending = 0;
	s.blocked = 0;
}

void sigInit(ProcSignals& s) {
	s.pending = 0;
	s.restart = 0;
	s.restorer = 0;
	for (int i = 0; i < NANOS_NSIG; i++)
		s.handlers[i] = kSigDefault;
}

// Posting a signal sets its pending bit. SIGCONT and the stop signals are mutually
// exclusive: a pending SIGCONT cancels pending stops and vice versa (Linux semantics).
// The interplay is applied within whichever pending word is posted to (thread- vs
// process-directed); since with one thread per process every async signal is process-
// directed, the two words never disagree in practice.
static void postBit(SigMask& pending, int sig) {
	if (sig == SIGCONT)
		// Cancel every pending stop signal, matching Linux's SIG_KERNEL_STOP_MASK
		// {SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU} — not just the two job-control keys.
		pending &= ~(bit(SIGSTOP) | bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU));
	else if (sigDefaultAction(sig) == SD_STOP)
		pending &= ~bit(SIGCONT);
	pending |= bit(sig);
}

void sigPost(ThreadSignals& s, int sig) {
	if (valid(sig))
		postBit(s.pending, sig);
}

void sigPost(ProcSignals& s, int sig) {
	if (valid(sig))
		postBit(s.pending, sig);
}

void sigConsume(ThreadSignals& ts, ProcSignals& ps, int sig) {
	if (valid(sig)) {
		ts.pending &= ~bit(sig);
		ps.pending &= ~bit(sig);
	}
}

// Lowest-numbered deliverable signal over the union of thread- and process-directed
// pending. SIGKILL/SIGSTOP cannot be blocked.
int sigNextDeliverable(const ThreadSignals& ts, const ProcSignals& ps) {
	SigMask pending = ts.pending | ps.pending;
	SigMask ready = pending & ~ts.blocked;
	ready |= pending & (bit(SIGKILL) | bit(SIGSTOP));   // these ignore the mask
	for (int sig = 1; sig < NANOS_NSIG; sig++)
		if (ready & bit(sig))
			return sig;
	return 0;
}

SigDisp sigResolve(const ProcSignals& ps, int sig) {
	// SIGKILL/SIGSTOP are immune to handlers.
	if (sig == SIGKILL)
		return DISP_TERM;
	if (sig == SIGSTOP)
		return DISP_STOP;

	unsigned h = valid(sig) ? ps.handlers[sig] : kSigDefault;
	if (h == kSigIgnore)
		return DISP_IGN;
	if (h != kSigDefault)
		return DISP_HANDLER;

	switch (sigDefaultAction(sig)) {
	case SD_IGN:  return DISP_IGN;
	case SD_STOP: return DISP_STOP;
	case SD_CONT: return DISP_CONT;
	default:      return DISP_TERM;   // SD_TERM / SD_CORE
	}
}

bool sigHasInterrupt(const ThreadSignals& ts, const ProcSignals& ps) {
	SigMask pending = ts.pending | ps.pending;
	SigMask ready = (pending & ~ts.blocked)
	               | (pending & (bit(SIGKILL) | bit(SIGSTOP)));   // these ignore the mask
	for (int sig = 1; sig < NANOS_NSIG; sig++) {
		if (!(ready & bit(sig)))
			continue;
		SigDisp d = sigResolve(ps, sig);
		if (d == DISP_TERM || d == DISP_STOP || d == DISP_HANDLER)
			return true;
	}
	return false;
}

void sigForkInherit(ProcSignals& child, const ProcSignals& parent) {
	child.restart = parent.restart;
	child.restorer = parent.restorer;
	child.pending = 0;                        // pending signals are NOT inherited
	for (int i = 0; i < NANOS_NSIG; i++)
		child.handlers[i] = parent.handlers[i];
}

void sigForkInherit(ThreadSignals& child, const ThreadSignals& parent) {
	child.blocked = parent.blocked;           // the forking thread's mask is inherited
	child.pending = 0;                        // pending signals are NOT inherited
}

// execve keeps ignored signals ignored but resets caught ones to the default; pending
// signals are dropped (both the process-directed set and the calling thread's). The
// block mask is preserved (Linux preserves it across exec).
void sigExecReset(ThreadSignals& ts, ProcSignals& ps) {
	ps.pending = 0;
	ts.pending = 0;
	for (int i = 0; i < NANOS_NSIG; i++)
		if (ps.handlers[i] != kSigIgnore)
			ps.handlers[i] = kSigDefault;
	ps.restart = 0;                           // caught handlers are gone -> no restart flags
}

int waitStatusExited(int code)    { return (code & 0xFF) << 8; }
int waitStatusSignalled(int sig)  { return sig & 0x7F; }
int waitStatusStopped(int sig)    { return ((sig & 0xFF) << 8) | 0x7F; }

}  // namespace kernel
